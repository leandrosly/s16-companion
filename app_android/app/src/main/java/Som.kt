package systems.leandro.s16

// Toca os sons do motor e as buzinas no celular (AudioTrack), com a velocidade vinda da roda.
//
// Quem gera o som e a Mesa (SomMotor.kt); aqui fica so a parte do Android:
//  * uma thread de audio que pede blocos a Mesa e escreve no AudioTrack
//  * a velocidade: da roda (pelo C3 ou direto na roda) quando os dados estao chegando,
//    senao a simulacao da propria tela (barra de velocidade / passeio automatico)
//  * os ajustes guardados nas preferencias
//
// O som continua com a tela apagada porque o ConexaoService (notificacao) fica ligado
// enquanto o motor estiver ligado, mesmo sem conexao nenhuma.

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.os.Process
import android.os.SystemClock
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow

/** O que a tela mostra (atualizado umas 10 vezes por segundo). */
data class PainelSom(
    val velocidade: Float = 0f,
    val rpm: Float = 0f,
    val marcha: Int = 0,
    val daRoda: Boolean = false,       // a velocidade esta vindo da roda
)

data class AjustesSom(
    val motor: String = MOTORES[0].id,
    val volume: Float = 0.7f,          // 0..1
    val timbre: Float = 0f,            // -12..+12 dB
    val reacao: Float = 1f,            // 0,5..3
    val marchas: Boolean = true,
    val estouros: Boolean = true,
    val pneu: Boolean = true,          // pneu cantando nas freadas fortes
    val simular: Boolean = false,      // usa a simulacao mesmo com a roda conectada
    val ligarJunto: Boolean = false,   // liga o motor sozinho quando conectar
)

object Som {
    private const val TAG = "Som"
    private const val PREFS = "s16_som"

    val ligado = MutableStateFlow(false)
    val painel = MutableStateFlow(PainelSom())
    val ajustes = MutableStateFlow(AjustesSom())
    /** Controles da simulacao (a tela mexe direto). */
    val alvoSim = MutableStateFlow(0f)
    val autoSim = MutableStateFlow(false)

    private lateinit var app: Context
    private var iniciado = false
    @Volatile private var mesa: Mesa? = null
    private var thread: Thread? = null
    private val trava = Object()
    private val pendentes = ArrayList<(Mesa) -> Unit>()    // comandos antes da Mesa existir
    private var sons: Map<String, Amostra>? = null

    // gravacoes em res/raw (mesmos nomes do laboratorio)
    private val RAW = mapOf(
        "h_e_lenta" to R.raw.h_e_lenta, "h_e_1600" to R.raw.h_e_1600, "h_e_1900" to R.raw.h_e_1900,
        "h_e_4000" to R.raw.h_e_4000, "h_e_partida" to R.raw.h_e_partida, "h_e_desliga" to R.raw.h_e_desliga,
        "h_estouros" to R.raw.h_estouros, "h_lenta" to R.raw.h_lenta, "h_partida1" to R.raw.h_partida1,
        "h_partida2" to R.raw.h_partida2, "h_desliga" to R.raw.h_desliga,
        "b_ahooga" to R.raw.b_ahooga, "b_vintage" to R.raw.b_vintage, "b_mustang" to R.raw.b_mustang,
        "b_mega" to R.raw.b_mega, "b_trem" to R.raw.b_trem, "b_bulb" to R.raw.b_bulb, "b_bici" to R.raw.b_bici,
        "p_pneu1" to R.raw.p_pneu1, "p_pneu2" to R.raw.p_pneu2,
    )

    fun iniciar(ctx: Context) {
        if (iniciado) return
        app = ctx.applicationContext
        iniciado = true
        val p = app.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val padrao = AjustesSom()
        ajustes.value = AjustesSom(
            motor = p.getString("motor", padrao.motor)?.takeIf { id -> MOTORES.any { it.id == id } } ?: padrao.motor,
            volume = p.getFloat("volume", padrao.volume),
            timbre = p.getFloat("timbre", padrao.timbre),
            reacao = p.getFloat("reacao", padrao.reacao),
            marchas = p.getBoolean("marchas", padrao.marchas),
            estouros = p.getBoolean("estouros", padrao.estouros),
            pneu = p.getBoolean("pneu", padrao.pneu),
            simular = p.getBoolean("simular", padrao.simular),
            ligarJunto = p.getBoolean("ligarJunto", padrao.ligarJunto),
        )
    }

    fun ajustar(novo: AjustesSom) {
        ajustes.value = novo
        if (iniciado) {
            app.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putString("motor", novo.motor).putFloat("volume", novo.volume).putFloat("timbre", novo.timbre)
                .putFloat("reacao", novo.reacao).putBoolean("marchas", novo.marchas).putBoolean("estouros", novo.estouros).putBoolean("pneu", novo.pneu)
                .putBoolean("simular", novo.simular).putBoolean("ligarJunto", novo.ligarJunto)
                .apply()
        }
        seAtivo { m -> aplica(m, novo) }                // (se o audio estiver parado, aplica ao abrir)
    }

    private fun aplica(m: Mesa, a: AjustesSom) {
        m.volume = a.volume; m.timbre = a.timbre; m.reacao = a.reacao
        m.marchas = a.marchas; m.estouros = a.estouros; m.pneuCantando = a.pneu
        if (m.motorId != a.motor) m.escolher(a.motor)
    }

    fun ligar(ctx: Context) {
        iniciar(ctx)
        if (ligado.value) return
        ligado.value = true
        ConexaoService.iniciar(app)                    // segura o app vivo com a tela apagada
        manda { it.ligar() }
    }

    fun desligar() {
        if (!ligado.value) return
        ligado.value = false
        manda { it.desligar() }
        if (iniciado && !C3Link.obter(app).querendoConexao) ConexaoService.parar(app)
    }

    fun alternar(ctx: Context) { if (ligado.value) desligar() else ligar(ctx) }

    fun apertaBuzina(ctx: Context, id: String) { iniciar(ctx); manda { it.apertaBuzina(id) } }
    fun soltaBuzina() { if (iniciado) manda { it.soltaBuzina() } }

    /** Buzina rapida (botao da notificacao): toca o ataque, um pedaco do laco e o final. */
    fun buzinaRapida(ctx: Context, id: String = BUZINAS[0].chave) {
        iniciar(ctx)
        manda { it.apertaBuzina(id) }
        manda { it.soltaBuzina() }
    }

    fun segurar(acel: Boolean, freio: Boolean) { seAtivo { it.segAcel = acel; it.segFreio = freio } }

    // ---------------- thread de audio ----------------
    /** Manda so se o audio ja estiver rodando (nao liga o audio a toa). */
    private fun seAtivo(c: (Mesa) -> Unit) {
        if (!iniciado) return
        synchronized(trava) {
            if (thread?.isAlive != true) return
            val m = mesa
            if (m != null) m.manda(c) else pendentes.add(c)
        }
    }

    private fun manda(c: (Mesa) -> Unit) {
        if (!iniciado) return
        synchronized(trava) {
            val m = mesa
            if (m != null && thread?.isAlive == true) m.manda(c) else pendentes.add(c)
            if (thread?.isAlive != true) {
                thread = Thread({ roda() }, "S16-som").also { it.start() }
            }
        }
    }

    private fun carregaSons(): Map<String, Amostra> {
        sons?.let { return it }
        val t0 = SystemClock.elapsedRealtime()
        val m = HashMap<String, Amostra>()
        for ((nome, id) in RAW) {
            try {
                app.resources.openRawResource(id).use { m[nome] = leWav(it.readBytes()) }
            } catch (e: Exception) {
                Log.w(TAG, "nao consegui ler $nome: $e")
            }
        }
        Log.i(TAG, "gravacoes carregadas em ${SystemClock.elapsedRealtime() - t0} ms")
        sons = m
        return m
    }

    private fun roda() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
        var track: AudioTrack? = null
        try {
            val sr = AudioTrack.getNativeOutputSampleRate(AudioManager.STREAM_MUSIC).takeIf { it in 8000..192000 } ?: 48000
            val m = Mesa(sr.toFloat(), carregaSons())
            aplica(m, ajustes.value)
            synchronized(trava) {
                for (c in pendentes) m.manda(c)
                pendentes.clear()
                mesa = m
            }
            val minimo = AudioTrack.getMinBufferSize(sr, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_FLOAT)
            val t = AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                        .build()
                )
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                        .setSampleRate(sr)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                        .build()
                )
                .setBufferSizeInBytes(maxOf(minimo, sr / 25 * 8))     // ~40 ms
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
            track = t
            t.play()
            val quadros = 256
            val buf = FloatArray(quadros * 2)
            val link = C3Link.obter(app)
            var silencio = 0
            var proxPainel = 0L
            var alvoEnviado = Float.NaN
            while (true) {
                // velocidade: da roda quando os dados estao chegando (e nao pediram simulacao)
                val r = link.roda.value
                val agora = System.currentTimeMillis()
                val daRoda = !ajustes.value.simular && r.quando != 0L && agora - r.quando < 1_500
                if (!daRoda) {
                    val alvo = alvoSim.value
                    if (alvo != alvoEnviado) { m.alvo = alvo; alvoEnviado = alvo }   // mexeram na barra
                    m.auto = autoSim.value
                }
                m.gera(buf, quadros, if (daRoda) r.velocidade else Float.NaN, if (daRoda) r.corrente else Float.NaN, r.quando)
                t.write(buf, 0, buf.size, AudioTrack.WRITE_BLOCKING)

                if (agora >= proxPainel) {
                    proxPainel = agora + 100
                    painel.value = PainelSom(m.v, m.rpm, m.marcha, daRoda)
                    if (!daRoda && m.alvo != alvoEnviado) {          // passeio ou acelerar/frear: a barra acompanha
                        alvoEnviado = m.alvo; alvoSim.value = m.alvo
                    }
                }
                // nada tocando por ~2 s: fecha o audio (volta sozinho no proximo comando)
                if (m.tocando) silencio = 0 else silencio += quadros
                if (silencio > sr * 2) {
                    synchronized(trava) {
                        if (!m.tocando && pendentes.isEmpty()) { mesa = null; thread = null; return }
                    }
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "audio parou: $e")
            synchronized(trava) { mesa = null; thread = null }
            ligado.value = false
        } finally {
            try { track?.stop() } catch (_: Exception) {}
            track?.release()
        }
    }
}
