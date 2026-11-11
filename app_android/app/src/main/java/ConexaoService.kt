package systems.leandro.s16

// Servico em primeiro plano: mantem a conexao com o C3 (ou com a roda) e o som do motor
// funcionando com o app fechado ou a tela apagada, e mostra uma notificacao fixa com
// velocidade, bateria e os botoes
//   [◀ Pisca]  [Buzina]  [Pisca ▶]          (conectado)
//   [Buzina]  [Desligar som]                (so o som, sem conexao)
// Ele liga quando o usuario conecta (C3Link.conectar) ou liga o som (Som.ligar), e desliga
// quando os dois acabam ("Desconectar" com o som desligado, ou desligar o som desconectado).
// A buzina da notificacao pode ser a da roda, a externa (433) ou a do celular: escolhida na pagina Roda.

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

class ConexaoService : Service() {

    companion object {
        private const val TAG = "ConexaoService"
        private const val CANAL = "conexao"
        private const val ID_NOTIFICACAO = 16
        private const val ACAO_PISCA_ESQ = "systems.leandro.s16.PISCA_ESQ"
        private const val ACAO_PISCA_DIR = "systems.leandro.s16.PISCA_DIR"
        private const val ACAO_BUZINA = "systems.leandro.s16.BUZINA"
        private const val ACAO_SOM = "systems.leandro.s16.SOM"

        /** Preferencia: qual buzina o botao da notificacao toca ("roda", "externa" ou "celular"). */
        const val PREF_BUZINA = "buzina_notif"
        /** Preferencia: qual gravacao a buzina do celular toca. */
        const val PREF_BUZINA_CEL = "buzina_celular"

        fun iniciar(ctx: Context) {
            try {
                ContextCompat.startForegroundService(ctx, Intent(ctx, ConexaoService::class.java))
            } catch (e: Exception) {
                Log.w(TAG, "nao consegui iniciar o servico: $e")
            }
        }

        fun parar(ctx: Context) {
            ctx.stopService(Intent(ctx, ConexaoService::class.java))
        }
    }

    private val escopo = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private lateinit var link: C3Link

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        link = C3Link.obter(this)
        Som.iniciar(this)
        criarCanal()
        entrarEmPrimeiroPlano()

        // Se o Android matou o app e recriou o servico, a conexao se perdeu: reconecta
        val prefs = getSharedPreferences("s16", Context.MODE_PRIVATE)
        val querConexao = prefs.getBoolean(C3Link.PREF_QUER, true)
        if (!link.querendoConexao && querConexao) {
            prefs.getString("mac_c3", null)?.let { link.reconectar(it) }
        }
        if (!link.querendoConexao && !querConexao && !Som.ligado.value) { stopSelf(); return }

        // atualiza a notificacao quando a conexao, o estado do C3 ou o som mudam, e a cada 1 s (velocidade)
        escopo.launch { combine(link.conexao, link.estado, Som.ligado) { _, _, _ -> }.collect { atualizar() } }
        escopo.launch { while (isActive) { delay(1_000); atualizar() } }
        // "Ligar o som junto com a conexao"
        escopo.launch {
            link.conexao.collect { c ->
                if (c == Conexao.CONECTADO && Som.ajustes.value.ligarJunto) Som.ligar(this@ConexaoService)
            }
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        entrarEmPrimeiroPlano()                                  // cada inicio precisa confirmar
        when (intent?.action) {
            ACAO_PISCA_ESQ -> link.enviar(Cmd.PISCA, 1)
            ACAO_PISCA_DIR -> link.enviar(Cmd.PISCA, 2)
            ACAO_BUZINA -> {
                val prefs = getSharedPreferences("s16", Context.MODE_PRIVATE)
                val qual = if (link.conexao.value == Conexao.CONECTADO) prefs.getString(PREF_BUZINA, "roda") else "celular"
                when (qual) {
                    "externa" -> link.enviar(Cmd.BUZEXT, 0xAA)
                    "celular" -> Som.buzinaRapida(this, prefs.getString(PREF_BUZINA_CEL, null) ?: BUZINAS[0].id)
                    else -> link.enviarRoda(0x88)
                }
            }
            ACAO_SOM -> Som.alternar(this)
        }
        return START_STICKY
    }

    override fun onDestroy() {
        escopo.cancel()
        super.onDestroy()
    }

    private fun entrarEmPrimeiroPlano() {
        val n = montar()
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                // "dispositivo conectado" (Bluetooth) so vale com a permissao; "midia" e o som do motor
                val bt = Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
                    ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
                var tipo = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
                if (bt) tipo = tipo or ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
                startForeground(ID_NOTIFICACAO, n, tipo)
            } else {
                startForeground(ID_NOTIFICACAO, n)
            }
        } catch (e: Exception) {
            Log.w(TAG, "startForeground: $e")
        }
    }

    private fun criarCanal() {
        val canal = NotificationChannel(CANAL, "Conexão com a roda", NotificationManager.IMPORTANCE_LOW).apply {
            description = "Velocidade, bateria, som do motor e botões de pisca e buzina"
            setShowBadge(false)
            lockscreenVisibility = Notification.VISIBILITY_PUBLIC
        }
        getSystemService(NotificationManager::class.java).createNotificationChannel(canal)
    }

    private fun atualizar() {
        try {
            getSystemService(NotificationManager::class.java).notify(ID_NOTIFICACAO, montar())
        } catch (e: Exception) {
            Log.w(TAG, "notify: $e")
        }
    }

    private fun acao(acao: String, codigo: Int): PendingIntent =
        PendingIntent.getForegroundService(
            this, codigo, Intent(this, ConexaoService::class.java).setAction(acao),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )

    private fun montar(): Notification {
        val con = link.conexao.value
        val est = link.estado.value
        val r = link.roda.value
        val temDados = r.quando != 0L && System.currentTimeMillis() - r.quando < 3_000

        val som = Som.ligado.value
        val nomeSom = MOTORES.firstOrNull { it.id == Som.ajustes.value.motor }?.nome ?: ""
        val soSom = som && con != Conexao.CONECTADO && !link.querendoConexao
        // (o "%" fica fora do format, como na tela da roda)
        val titulo = when {
            soSom -> "Som: $nomeSom"
            con == Conexao.CONECTADO -> if (temDados) "%.1f km/h".format(r.velocidade) + " · bateria ${r.bateria}%"
                                        else if (link.direto.value) "Conectado na roda" else "Conectado ao C3"
            con == Conexao.CONECTANDO -> "Conectando..."
            else -> "Desconectado: tentando de novo..."
        }
        val texto = when {
            soSom -> "Simulação · " + "%.0f km/h".format(Som.painel.value.velocidade)
            con != Conexao.CONECTADO -> if (som) "Som: $nomeSom · ligue a roda" else "Ligue a roda"
            est.pisca == 1 -> "◀ Pisca esquerdo ligado"
            est.pisca == 2 -> "Pisca direito ligado ▶"
            est.freio -> "Freando"
            som -> "Som: $nomeSom"
            else -> "Toque para abrir o S16 Companion"
        }
        val abrir = PendingIntent.getActivity(
            this, 0,
            Intent(this, PainelActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val b = NotificationCompat.Builder(this, CANAL)
            .setSmallIcon(R.drawable.ic_notificacao)
            .setContentTitle(titulo)
            .setContentText(texto)
            .setContentIntent(abrir)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setSilent(true)
            .setShowWhen(false)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
        // piscas so com o C3; sem ele: buzina (da roda ou do celular) e o som
        if (con == Conexao.CONECTADO && !link.direto.value) {
            b.addAction(0, if (est.pisca == 1) "◀ PISCA" else "◀ Pisca", acao(ACAO_PISCA_ESQ, 1))
            b.addAction(0, "Buzina", acao(ACAO_BUZINA, 2))
            b.addAction(0, if (est.pisca == 2) "PISCA ▶" else "Pisca ▶", acao(ACAO_PISCA_DIR, 3))
        } else {
            b.addAction(0, "Buzina", acao(ACAO_BUZINA, 2))
            b.addAction(0, if (som) "Desligar som" else "Ligar som", acao(ACAO_SOM, 4))
        }
        return b.build()
    }
}
