package systems.leandro.s16

// Conexao BLE com o C3 que fica dentro da roda.
//
// O C3 e um "clone" da roda: tem o mesmo servico FFE0 e a mesma caracteristica FFE1.
// Os apps (EUC World etc.) conversam por ali com a roda; o nosso app usa o mesmo caminho,
// mas com quadros marcados que o C3 executa e NAO repassa para a roda:
//
//   app -> C3:  AA 55 01 [cmd] [arg] 00 ... 00 EE 53 31 36          (20 bytes)
//   C3 -> app:  AA 55 02 [luz] [pisca] [freio] [freioAuto] [teste433] [botao] [seq]
//               [radioOk] [apps] [ledModo] [ledBrilho] [ledCor] [ledFlags] EE 53 31 36
//
// O primeiro comando e um HELLO com arg 1 ("sou o app"); a partir dai o C3 manda o estado
// sempre que algo muda e a cada 2 s.
//
// Detalhe do Android: se o EUC World ja estiver conectado no C3, o nosso app usa a MESMA
// conexao (o Android compartilha). Por isso o app nunca desliga as notificacoes da FFE1:
// isso cortaria os dados da roda para o EUC World.

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import java.util.UUID

/** O que o C3 informa (bytes 3..15 do quadro de estado). */
data class EstadoC3(
    val recebido: Boolean = false,      // ja chegou pelo menos um quadro de estado
    val luzBase: Int = 0,               // lanternas paradas: 0 apagadas, 1 laranja, 2 vermelhas, 3 piscando
    val pisca: Int = 0,                 // 0 nenhum, 1 esquerda, 2 direita
    val freio: Boolean = false,         // luz de freio acesa agora
    val freioAuto: Boolean = false,     // freio automatico pela desaceleracao da roda
    val teste433: Boolean = false,      // rajadas de teste do 433 ligadas
    val evBotao: Int = 0,               // ultimo botao do controle 433
    val evSeq: Int = 0,                 // contador de apertos (muda a cada botao)
    val radioOk: Boolean = false,       // transmissor 433 funcionando
    val apps: Int = 0,                  // conexoes de apps no C3 (sem contar o display)
    val ledModo: Int = 4,               // fitas: 0 apagadas, 1 fixa, 2 respira, 3 arco-iris, 4 auto
    val ledBrilho: Int = 40,            // 0..60 (%)
    val ledCor: Int = 6,                // indice da paleta (255 = cor fora da paleta)
    val ledPiscas: Boolean = true,      // piscas aparecem nas fitas
    val ledFreio: Boolean = true,       // freio aparece nas fitas
    val quando: Long = 0,               // hora em que chegou (ms)
)

/** Dados da roda, tirados dos mesmos quadros que o EUC World recebe (repassados pelo C3). */
data class DadosRoda(
    val tensao: Float = 0f,             // V
    val velocidade: Float = 0f,         // km/h
    val corrente: Float = 0f,           // A (negativa = regenerando)
    val temperatura: Float = 0f,        // graus C (placa)
    val tripKm: Float = 0f,             // km desde que ligou
    val maxima: Float = 0f,             // km/h maxima da viagem
    val pwm: Int = -1,                  // % de uso do motor
    val limite: Float = 0f,             // km/h (alarme de velocidade da roda)
    val soc: Float = -1f,               // % da bateria pelo BMS (-1 = nao chegou)
    val quando: Long = 0,               // hora do ultimo quadro A9 (ms)
) {
    /** % da bateria: o do BMS quando chega; senao uma estimativa pela tensao (20 celulas, 3,3 a 4,2 V). */
    val bateria: Int get() =
        if (soc >= 0) soc.toInt() else ((tensao / 20f - 3.3f) / 0.9f * 100f).coerceIn(0f, 100f).toInt()
    val bateriaPeloBms: Boolean get() = soc >= 0
}

enum class Conexao { DESCONECTADO, PROCURANDO, CONECTANDO, CONECTADO, DEMO }   // DEMO = sem C3, so para ver as telas

data class Achado(val nome: String, val mac: String, val rssi: Int)

/** Comandos do canal (iguais ao enum do ponte_c3.ino). */
object Cmd {
    const val HELLO = 1
    const val PISCA = 2
    const val LUZBASE = 3
    const val BUZEXT = 4
    const val FREIOAUTO = 5
    const val FREIOTESTE = 6
    const val TESTE433 = 7
    const val LED_MODO = 8
    const val LED_BRILHO = 9
    const val LED_COR = 10
    const val LED_FLAGS = 11
}

/** MAC da roda de verdade: aparece na busca quando o C3 nao esta conectado nela. */
const val MAC_RODA = "C5:39:32:38:4C:4B"

@SuppressLint("MissingPermission")   // as permissoes sao pedidas na PainelActivity antes de usar
class C3Link private constructor(private val ctx: Context) {

    companion object {
        private const val TAG = "C3Link"
        private val SERVICO: UUID = UUID.fromString("0000ffe0-0000-1000-8000-00805f9b34fb")
        private val CARAC: UUID = UUID.fromString("0000ffe1-0000-1000-8000-00805f9b34fb")
        private val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        private val MARCA = byteArrayOf(0xEE.toByte(), 0x53, 0x31, 0x36)
        /** Preferencia: o usuario quer ficar conectado (o servico reconecta se for recriado). */
        const val PREF_QUER = "quer_conexao"

        // Uma conexao so para o app inteiro (sobrevive a girar a tela)
        @Volatile private var unico: C3Link? = null
        fun obter(ctx: Context): C3Link =
            unico ?: synchronized(this) { unico ?: C3Link(ctx.applicationContext).also { unico = it } }
    }

    val estado = MutableStateFlow(EstadoC3())
    val roda = MutableStateFlow(DadosRoda())
    private val socPack = floatArrayOf(-1f, -1f)
    val conexao = MutableStateFlow(Conexao.DESCONECTADO)
    /** Conectado direto na roda (sem C3): chegam dados da roda, mas o C3 nunca respondeu. */
    val direto = MutableStateFlow(false)
    private var quandoPronto = 0L
    val achados = MutableStateFlow<List<Achado>>(emptyList())

    private val adaptador: BluetoothAdapter? =
        (ctx.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
    private val main = Handler(Looper.getMainLooper())
    private fun prefs() = ctx.getSharedPreferences("s16", Context.MODE_PRIVATE)

    // tudo abaixo so e mexido na thread principal (os callbacks do Bluetooth fazem main.post)
    private var gatt: BluetoothGatt? = null
    private var carac: BluetoothGattCharacteristic? = null
    private val fila = ArrayDeque<ByteArray>()
    private var ocupado = false
    private var tokenEscrita = 0
    private var querConectado = false
    /** true depois de um conectar/reconectar, ate o "Desconectar". */
    val querendoConexao: Boolean get() = querConectado
    private var macAtual: String? = null

    // ---------------- busca ----------------
    private val scanCb = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, r: ScanResult) {
            val nome = r.scanRecord?.deviceName ?: r.device.name ?: return
            val temFfe0 = r.scanRecord?.serviceUuids?.any { it.uuid == SERVICO } == true
            if (!nome.startsWith("KS") && !temFfe0) return
            val mac = r.device.address
            val lista = achados.value.filter { it.mac != mac } + Achado(nome, mac, r.rssi)
            achados.value = lista.sortedByDescending { it.rssi }
        }
    }

    fun procurar() {
        val sc = adaptador?.bluetoothLeScanner ?: return
        if (conexao.value == Conexao.DEMO) sairDemo()
        if (conexao.value == Conexao.CONECTADO || conexao.value == Conexao.CONECTANDO) desconectar()
        achados.value = emptyList()
        conexao.value = Conexao.PROCURANDO
        val cfg = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        try { sc.startScan(null, cfg, scanCb) } catch (e: Exception) { Log.w(TAG, "startScan: $e") }
        main.postDelayed({ pararBusca() }, 10_000)
    }

    fun pararBusca() {
        try { adaptador?.bluetoothLeScanner?.stopScan(scanCb) } catch (_: Exception) {}
        if (conexao.value == Conexao.PROCURANDO) conexao.value = Conexao.DESCONECTADO
    }

    // ---------------- conexao ----------------
    /** Conecta pedido pelo usuario: tambem liga o servico (notificacao + conexao com a tela apagada). */
    fun conectar(mac: String) {
        prefs().edit().putBoolean(PREF_QUER, true).apply()
        ConexaoService.iniciar(ctx)
        reconectar(mac)
    }

    /** Conecta sem mexer no servico (usado nas novas tentativas e pelo proprio servico). */
    fun reconectar(mac: String) {
        querConectado = true                                     // ja marca (o servico consulta isto)
        macAtual = mac
        main.post {
            if (conexao.value == Conexao.DEMO) sairDemo()
            pararBusca()
            achados.value = emptyList()
            macAtual = mac
            querConectado = true
            val dev: BluetoothDevice = adaptador?.getRemoteDevice(mac) ?: return@post
            gatt?.close()
            carac = null
            conexao.value = Conexao.CONECTANDO
            gatt = dev.connectGatt(ctx, false, gattCb, BluetoothDevice.TRANSPORT_LE)
        }
    }

    fun desconectar() {
        querConectado = false
        prefs().edit().putBoolean(PREF_QUER, false).apply()
        if (!Som.ligado.value) ConexaoService.parar(ctx)        // com o som ligado, o servico continua
        main.post {
            querConectado = false
            gatt?.disconnect()
            gatt?.close()
            gatt = null
            carac = null
            fila.clear()
            ocupado = false
            direto.value = false
            conexao.value = Conexao.DESCONECTADO
        }
    }

    private val gattCb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, novo: Int) {
            main.post {
                if (g != gatt) return@post                       // conexao antiga
                if (novo == BluetoothProfile.STATE_CONNECTED) {
                    Log.i(TAG, "conectado, procurando servicos")
                    g.discoverServices()
                } else if (novo == BluetoothProfile.STATE_DISCONNECTED) {
                    Log.i(TAG, "desconectado (status $status)")
                    g.close()
                    gatt = null
                    carac = null
                    fila.clear()
                    ocupado = false
                    direto.value = false
                    conexao.value = Conexao.DESCONECTADO
                    if (querConectado) {                         // roda desligou? tenta de novo
                        main.postDelayed({
                            if (querConectado && gatt == null) macAtual?.let { reconectar(it) }
                        }, 3_000)
                    }
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            main.post {
                if (g != gatt) return@post
                val c = g.getService(SERVICO)?.getCharacteristic(CARAC)
                if (c == null) {
                    Log.w(TAG, "FFE1 nao encontrada: isto nao e o C3?")
                    g.disconnect()
                    return@post
                }
                carac = c
                g.setCharacteristicNotification(c, true)
                val d = c.getDescriptor(CCCD)
                if (d == null) aposAssinar() else escreverCccd(g, d)
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            main.post { if (g == gatt) aposAssinar() }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            Log.i(TAG, "MTU $mtu")
            main.post { if (g == gatt) pronto() }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            main.post { ocupado = false; proximo() }
        }

        // Android 13 em diante
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            recebeu(value)
        }

        // Android 12 e anteriores
        @Suppress("OVERRIDE_DEPRECATION", "DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT < 33) c.value?.let { recebeu(it.copyOf()) }
        }
    }

    @Suppress("DEPRECATION")
    private fun escreverCccd(g: BluetoothGatt, d: BluetoothGattDescriptor) {
        val v = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        if (Build.VERSION.SDK_INT >= 33) {
            g.writeDescriptor(d, v)
        } else {
            d.value = v                                          // jeito antigo (ate o Android 12)
            g.writeDescriptor(d)
        }
    }

    // Pede pacotes maiores (os quadros do BMS passam de 20 bytes); depois manda o HELLO.
    // Se o EUC World ja tiver pedido, o Android so confirma o que ja esta valendo.
    private fun aposAssinar() {
        val g = gatt ?: return
        if (conexao.value == Conexao.CONECTADO) return
        if (!g.requestMtu(247)) { pronto(); return }
        main.postDelayed({ pronto() }, 1_500)                    // se a resposta nao vier
    }

    private fun pronto() {
        if (gatt == null || conexao.value == Conexao.CONECTADO) return
        Log.i(TAG, "pronto: mandando HELLO")
        quandoPronto = System.currentTimeMillis()
        direto.value = false
        estado.value = EstadoC3()                                // (o de uma conexao anterior nao vale)
        conexao.value = Conexao.CONECTADO
        enviar(Cmd.HELLO, 1)                                     // "sou o app": o C3 manda o estado
        enviarRoda(0x9B)                                         // pede o nome da roda (como os outros apps):
                                                                 // conectado direto na roda, faz ela comecar a mandar
    }

    // ---------------- envio ----------------
    /** Manda um comando do canal ao C3 (fila: o Android so aceita uma escrita por vez). */
    fun enviar(cmd: Int, arg: Int) {
        if (conexao.value == Conexao.DEMO) { simular(cmd, arg); return }
        val p = ByteArray(20)
        p[0] = 0xAA.toByte(); p[1] = 0x55; p[2] = 0x01
        p[3] = cmd.toByte(); p[4] = arg.toByte()
        for (i in 0..3) p[16 + i] = MARCA[i]
        main.post { fila.addLast(p); proximo() }
    }

    @Suppress("DEPRECATION")
    /**
     * Comando direto para a RODA, no formato da KingSong (o C3 repassa como faz com os apps):
     *   AA 55 [b2] [b3] 00 ... [16]=cmd 14 5A 5A
     */
    fun enviarRoda(cmd: Int, b2: Int = 0, b3: Int = 0) {
        if (conexao.value == Conexao.DEMO) return
        val p = ByteArray(20)
        p[0] = 0xAA.toByte(); p[1] = 0x55
        p[2] = b2.toByte(); p[3] = b3.toByte()
        p[16] = cmd.toByte(); p[17] = 0x14; p[18] = 0x5A; p[19] = 0x5A
        main.post { fila.addLast(p); proximo() }
    }

    private fun proximo() {
        val g = gatt ?: return
        val c = carac ?: return
        if (ocupado) return
        val d = fila.removeFirstOrNull() ?: return
        ocupado = true
        val meu = ++tokenEscrita
        val tipo = if ((c.properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE) != 0)
            BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE else BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        val ok = if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(c, d, tipo) == BluetoothStatusCodes.SUCCESS
        } else {
            c.writeType = tipo                                   // jeito antigo (ate o Android 12)
            c.value = d
            g.writeCharacteristic(c)
        }
        if (!ok) {                                               // ocupado no Android: tenta a fila depois
            Log.w(TAG, "escrita recusada, descartando ${d[3]}")
            ocupado = false
            main.postDelayed({ proximo() }, 100)
            return
        }
        // se o callback de escrita nao vier, nao trava a fila
        main.postDelayed({ if (ocupado && tokenEscrita == meu) { ocupado = false; proximo() } }, 600)
    }

    // ---------------- recepcao ----------------
    private fun recebeu(v: ByteArray) {
        if (v.size < 20 || v[0] != 0xAA.toByte() || v[1] != 0x55.toByte()) return
        var canal = true
        for (i in 0..3) if (v[16 + i] != MARCA[i]) canal = false
        if (!canal) { dadosDaRoda(v); return }
        if (v[2] != 0x02.toByte()) return
        if (direto.value) direto.value = false                   // o C3 respondeu, afinal
        fun u(i: Int) = v[i].toInt() and 0xFF
        estado.value = EstadoC3(
            recebido = true,
            luzBase = u(3), pisca = u(4), freio = u(5) != 0, freioAuto = u(6) != 0,
            teste433 = u(7) != 0, evBotao = u(8), evSeq = u(9), radioOk = u(10) != 0, apps = u(11),
            ledModo = u(12), ledBrilho = u(13), ledCor = u(14),
            ledPiscas = (u(15) and 1) != 0, ledFreio = (u(15) and 2) != 0,
            quando = System.currentTimeMillis(),
        )
    }

    // Quadros da KingSong (mesma leitura do display, ks_s16_ponte_v5_midia.ino: parseFrame)
    private fun dadosDaRoda(d: ByteArray) {
        fun u8(i: Int) = d[i].toInt() and 0xFF
        fun u16(i: Int) = u8(i) or (u8(i + 1) shl 8)
        fun s16(i: Int) = u16(i).toShort().toInt()
        fun u32r(i: Int) = (u16(i).toLong() shl 16) or u16(i + 2).toLong()
        val r = roda.value
        // dados da roda chegando e o C3 calado: estamos conectados direto na roda
        if (!direto.value && conexao.value == Conexao.CONECTADO && !estado.value.recebido &&
            System.currentTimeMillis() - quandoPronto > 2_500) {
            Log.i(TAG, "sem resposta do C3, mas a roda manda dados: conexao direta")
            direto.value = true
        }
        when (u8(16)) {
            0xA9 -> roda.value = r.copy(
                tensao = u16(2) / 100f, velocidade = u16(4) / 100f,
                corrente = s16(10) / 100f, temperatura = u16(12) / 100f,
                quando = System.currentTimeMillis(),
            )
            0xB9 -> roda.value = r.copy(tripKm = u32r(2) / 1000f, maxima = u16(8) / 100f)
            0xF6 -> roda.value = r.copy(limite = u16(2) / 100f)
            0xF5 -> roda.value = r.copy(pwm = u8(15))
            0xF1, 0xF2 -> if (d.size > 20 && u8(17) == 0xD0) {          // BMS de cada pacote
                val n = u8(21).coerceAtMost(24)
                val base = 22 + 2 * n
                if (d.size < base + 1) return
                val nt = u8(base).coerceAtMost(8)
                val depois = base + 1 + 2 * nt
                if (d.size < depois + 6) return
                socPack[if (u8(16) == 0xF1) 0 else 1] = u16(depois + 4) / 10f
                val validos = socPack.filter { it >= 0 }
                if (validos.isNotEmpty()) roda.value = r.copy(soc = validos.average().toFloat())
            }
        }
    }

    // ---------------- demonstracao (sem C3) ----------------
    /** Finge um C3 conectado: os comandos mudam um estado local, como o C3 faria. */
    fun entrarDemo() {
        if (!Som.ligado.value) ConexaoService.parar(ctx)
        main.post {
            pararBusca()
            querConectado = false
            gatt?.disconnect(); gatt?.close(); gatt = null; carac = null
            fila.clear(); ocupado = false
            estado.value = EstadoC3(recebido = true, radioOk = true, apps = 1, quando = System.currentTimeMillis())
            conexao.value = Conexao.DEMO
            main.post(demoRoda)
        }
    }

    // roda de mentirinha andando, so para ver a tela de dados
    private val demoRoda = object : Runnable {
        override fun run() {
            if (conexao.value != Conexao.DEMO) return
            val t = System.currentTimeMillis() / 1000.0
            val vel = (18 + 12 * Math.sin(t / 4)).toFloat()
            val r = roda.value
            roda.value = r.copy(
                tensao = 79.4f, velocidade = vel, corrente = (6 + 10 * Math.cos(t / 4)).toFloat(),
                temperatura = 38.5f, tripKm = 12.3f + (t % 1000).toFloat() / 200f,
                maxima = maxOf(r.maxima, vel), pwm = (vel * 1.4f).toInt(), limite = 45f, soc = 78f,
                quando = System.currentTimeMillis(),
            )
            main.postDelayed(this, 200)
        }
    }

    fun sairDemo() {
        if (conexao.value != Conexao.DEMO) return
        estado.value = EstadoC3()
        roda.value = DadosRoda()
        conexao.value = Conexao.DESCONECTADO
    }

    private fun simular(cmd: Int, arg: Int) {
        val e = estado.value
        estado.value = when (cmd) {
            Cmd.PISCA -> e.copy(pisca = if (e.pisca == arg) 0 else arg)
            Cmd.LUZBASE -> e.copy(luzBase = arg)
            Cmd.FREIOAUTO -> e.copy(freioAuto = arg != 0)
            Cmd.FREIOTESTE -> {
                main.postDelayed({ if (conexao.value == Conexao.DEMO) estado.value = estado.value.copy(freio = false) }, 2_500)
                e.copy(freio = true)
            }
            Cmd.LED_MODO -> e.copy(ledModo = arg)
            Cmd.LED_BRILHO -> e.copy(ledBrilho = arg.coerceAtMost(60))
            Cmd.LED_COR -> e.copy(ledCor = arg, ledModo = if (e.ledModo == 0) 1 else e.ledModo)
            Cmd.LED_FLAGS -> e.copy(ledPiscas = (arg and 1) != 0, ledFreio = (arg and 2) != 0)
            else -> e                                            // buzina etc.: nada para mostrar
        }.copy(quando = System.currentTimeMillis())
    }

    // Se o estado parar de chegar (C3 reiniciou e esqueceu o app), manda o HELLO de novo
    private val vigia = object : Runnable {
        override fun run() {
            if (conexao.value == Conexao.CONECTADO && !direto.value &&
                System.currentTimeMillis() - estado.value.quando > 5_000) {
                enviar(Cmd.HELLO, 1)
            }
            main.postDelayed(this, 3_000)
        }
    }

    init { main.postDelayed(vigia, 3_000) }
}
