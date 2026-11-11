package systems.leandro.s16

// Motor de som do app: os mesmos motores e buzinas do laboratorio_sons.html, refeitos amostra
// por amostra (sem Web Audio). Este arquivo nao usa nada do Android: quem toca e o Som.kt.
//
// Cadeia final (igual ao laboratorio):
//   motores + buzinas -> volume -> grave (250 Hz) -> agudo (2,2 kHz) -> compressor -> saida
//
// Detalhes de compatibilidade com o Web Audio (para soar igual ao laboratorio):
//  * passa-baixa e passa-alta: o "Q" do Web Audio e em dB (0,7 = 1,084 de Q de verdade)
//  * passa-faixa e pico: Q normal
//  * setTargetAtTime(alvo, t, tau) = classe Suave (aproximacao exponencial)
//  * fonte mono indo para a saida estereo = mesmo sinal nos dois lados (L = R)
//  * StereoPanner com fonte mono: L = cos((p+1)/2 * 90 graus), R = sen(...)

import java.util.concurrent.ConcurrentLinkedQueue
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.floor
import kotlin.math.ln
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.sin
import kotlin.math.sqrt
import kotlin.math.tanh
import kotlin.random.Random

const val BLOCO = 64                 // amostras por sub-bloco de mistura
private const val CONTROLE = 4       // sub-blocos por passo de controle (~5 ms a 48 kHz)
private const val DOIS_PI = 2.0 * PI

internal fun clampf(x: Float, a: Float, b: Float) = if (x < a) a else if (x > b) b else x
/** Limitador suave: igual ate 0,7; acima disso encosta devagar em 1 (sem estalar). */
internal fun limita(x: Float): Float {
    val ax = abs(x)
    if (ax <= 0.7f) return x
    val y = 0.7f + 0.3f * tanh((ax - 0.7f) / 0.3f)
    return if (x < 0) -y else y
}
internal fun dbLin(db: Float) = if (db <= -90f) 0f else 10f.pow(db / 20f)
private fun rnd() = Random.nextFloat()

/** Uma gravacao em memoria (mono, -1..1). */
class Amostra(val dados: FloatArray, val sr: Int) {
    val dur: Double get() = dados.size.toDouble() / sr
}

/** Tabela de seno (fase 0..1). */
private object Seno {
    private const val N = 4096
    private val t = FloatArray(N + 1) { sin(DOIS_PI * it / N).toFloat() }
    fun s(fase: Double): Float {
        val x = fase * N
        val i = x.toInt().coerceIn(0, N - 1)
        val fr = (x - i).toFloat()
        return t[i] + (t[i + 1] - t[i]) * fr
    }
}

/** Valor que vai ate o alvo exponencialmente (setTargetAtTime). */
class Suave(v: Float) {
    var valor = v
    var alvo = v
        private set
    private var tau = 0f
    fun para(a: Float, t: Float) { alvo = a; tau = t }
    fun ja(a: Float) { alvo = a; valor = a }
    /** Avanca n amostras e devolve o novo valor. */
    fun avanca(n: Int, sr: Float): Float {
        valor = if (tau <= 0f) alvo else alvo + (valor - alvo) * exp(-n / (tau * sr))
        return valor
    }
}

/** Oscilador: 's' seno, 'w' dente de serra, 'q' quadrada, 't' triangulo (com polyBLEP). */
class Osc(private val tipo: Char, private val sr: Float) {
    private var fase = 0.0
    fun prox(f: Float): Float {
        val dt = (f / sr).toDouble().coerceIn(0.0, 0.49)
        val p = fase
        fase += dt
        if (fase >= 1.0) fase -= 1.0
        return when (tipo) {
            's' -> Seno.s(p)
            'w' -> (2 * p - 1 - blep(p, dt)).toFloat()
            'q' -> {
                var y = if (p < 0.5) 1.0 else -1.0
                y += blep(p, dt)
                y -= blep((p + 0.5) % 1.0, dt)
                y.toFloat()
            }
            else -> (if (p < 0.5) 4 * p - 1 else 3 - 4 * p).toFloat()
        }
    }
    private fun blep(t: Double, dt: Double): Double = when {
        dt <= 0.0 -> 0.0
        t < dt -> { val x = t / dt; x + x - x * x - 1 }
        t > 1 - dt -> { val x = (t - 1) / dt; x * x + x + x + 1 }
        else -> 0.0
    }
}

/** Filtro biquad com as formulas do Web Audio. */
class Filtro(private val tipo: Int, var f: Float, var q: Float = 0.7f, var g: Float = 0f) {
    companion object {
        const val PB = 0; const val PA = 1; const val FAIXA = 2; const val PICO = 3; const val GRAVE = 4; const val AGUDO = 5
    }
    private var b0 = 1.0; private var b1 = 0.0; private var b2 = 0.0; private var a1 = 0.0; private var a2 = 0.0
    private var z1 = 0.0; private var z2 = 0.0
    private var fC = Float.NaN; private var qC = Float.NaN; private var gC = Float.NaN; private var srC = 0f

    /** Recalcula os coeficientes se algo mudou (chamar uma vez por sub-bloco). */
    fun ajusta(sr: Float) {
        if (abs(z1) < 1e-25) z1 = 0.0
        if (abs(z2) < 1e-25) z2 = 0.0
        if (f == fC && q == qC && g == gC && sr == srC) return
        fC = f; qC = q; gC = g; srC = sr
        val fr = clampf(f, 5f, sr * 0.4995f).toDouble()
        val w0 = DOIS_PI * fr / sr
        val cw = cos(w0); val sw = sin(w0)
        val A = 10.0.pow(g / 40.0)
        var nb0: Double; var nb1: Double; var nb2: Double; val na0: Double; val na1: Double; val na2: Double
        when (tipo) {
            PB, PA -> {
                val al = sw / (2 * 10.0.pow(q / 20.0))
                if (tipo == PB) { nb0 = (1 - cw) / 2; nb1 = 1 - cw; nb2 = (1 - cw) / 2 }
                else { nb0 = (1 + cw) / 2; nb1 = -(1 + cw); nb2 = (1 + cw) / 2 }
                na0 = 1 + al; na1 = -2 * cw; na2 = 1 - al
            }
            FAIXA -> {
                val al = sw / (2 * max(q, 1e-4f))
                nb0 = al; nb1 = 0.0; nb2 = -al
                na0 = 1 + al; na1 = -2 * cw; na2 = 1 - al
            }
            PICO -> {
                val al = sw / (2 * max(q, 1e-4f))
                nb0 = 1 + al * A; nb1 = -2 * cw; nb2 = 1 - al * A
                na0 = 1 + al / A; na1 = -2 * cw; na2 = 1 - al / A
            }
            else -> {
                val al = sw / 2 * sqrt(2.0)
                val k = 2 * sqrt(A) * al
                if (tipo == GRAVE) {
                    nb0 = A * ((A + 1) - (A - 1) * cw + k); nb1 = 2 * A * ((A - 1) - (A + 1) * cw); nb2 = A * ((A + 1) - (A - 1) * cw - k)
                    na0 = (A + 1) + (A - 1) * cw + k; na1 = -2 * ((A - 1) + (A + 1) * cw); na2 = (A + 1) + (A - 1) * cw - k
                } else {
                    nb0 = A * ((A + 1) + (A - 1) * cw + k); nb1 = -2 * A * ((A - 1) + (A + 1) * cw); nb2 = A * ((A + 1) + (A - 1) * cw - k)
                    na0 = (A + 1) - (A - 1) * cw + k; na1 = 2 * ((A - 1) - (A + 1) * cw); na2 = (A + 1) - (A - 1) * cw - k
                }
            }
        }
        b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0; a1 = na1 / na0; a2 = na2 / na0
    }

    fun proc(x: Float): Float {
        val y = b0 * x + z1
        z1 = b1 * x - a1 * y + z2
        z2 = b2 * x - a2 * y
        return y.toFloat()
    }
}

/** Cadeia de filtros em serie. */
class Cadeia(vararg val fs: Filtro) {
    fun ajusta(sr: Float) { for (f in fs) f.ajusta(sr) }
    fun proc(x: Float): Float { var y = x; for (f in fs) y = f.proc(y); return y }
}

/** Ruido gravado em laco (como um AudioBufferSourceNode com loop). */
class Laco(private val d: FloatArray, ini: Int) {
    private var i = ((ini % d.size) + d.size) % d.size
    fun prox(): Float { val y = d[i]; i++; if (i == d.size) i = 0; return y }
}

/** Eco com realimentacao (DelayNode + ganho de volta). */
class Eco(sr: Float, seg: Float, private val fb: Float) {
    private val d = FloatArray(max(1, (seg * sr).toInt()))
    private var i = 0
    /** Entra x, devolve a saida do atraso (o que vai para o "wet"). */
    fun proc(x: Float): Float {
        val y = d[i]
        var w = x + y * fb
        if (abs(w) < 1e-15f) w = 0f
        d[i] = w
        i++; if (i == d.size) i = 0
        return y
    }
}

/** Pan de fonte mono (StereoPannerNode). */
private fun panL(p: Float) = cos((p + 1) / 2 * PI / 2).toFloat()
private fun panR(p: Float) = sin((p + 1) / 2 * PI / 2).toFloat()

/** Toca uma gravacao: velocidade variavel, laco opcional, atraso, entrada e saida suaves. */
class Voz(private val a: Amostra, private val sr: Float) {
    var pos = 0.0                       // posicao na gravacao (amostras dela)
    val taxa = Suave(1f)
    val ganho = Suave(1f)
    var laco = false
    var lsA = 0.0                       // inicio do laco (amostras da gravacao)
    var leA = a.dados.size.toDouble()   // fim do laco
    var fimA = a.dados.size.toDouble()  // sem laco: para ao passar daqui
    var atraso = 0                      // amostras de saida antes de comecar
    var entrada = 0                     // rampa de entrada (amostras)
    private var nIn = 0
    private var saidaEm = -1            // contagem ate comecar a rampa de saida (-1 = nunca)
    private var saidaDur = 1
    private var nOut = 0
    var acabou = false
        private set
    private val passo = a.sr / sr

    fun comecaEm(seg: Double) { pos = seg * a.sr }
    fun lacoEm(ls: Double, le: Double) { laco = true; lsA = ls * a.sr; leA = le * a.sr }
    fun ateSeg(seg: Double) { fimA = min(a.dados.size.toDouble(), seg * a.sr) }
    /** Sai com uma rampa linear de durAmostras, depois de esperar emAmostras. */
    fun sai(emAmostras: Int, durAmostras: Int) {
        if (saidaEm >= 0) return
        saidaEm = max(0, emAmostras); saidaDur = max(1, durAmostras)
    }

    /** Soma em l (e em r, se dado). */
    fun soma(l: FloatArray, r: FloatArray?, n: Int) {
        if (acabou) return
        val g0 = ganho.valor; val g1 = ganho.avanca(n, sr); val dg = (g1 - g0) / n
        val t0 = taxa.valor; val t1 = taxa.avanca(n, sr); val dt = (t1 - t0) / n
        var g = g0; var tx = t0
        val d = a.dados
        val ult = d.size - 1
        for (i in 0 until n) {
            g += dg; tx += dt
            if (atraso > 0) { atraso--; continue }
            var env = 1f
            if (entrada > 0 && nIn < entrada) { env = nIn.toFloat() / entrada; nIn++ }
            if (saidaEm >= 0) {
                if (saidaEm > 0) saidaEm--
                else {
                    nOut++
                    env *= max(0f, 1f - nOut.toFloat() / saidaDur)
                    if (nOut >= saidaDur) { acabou = true; return }
                }
            }
            val ip = pos.toInt()
            if (ip >= ult) { if (!laco) { acabou = true; return } }
            val i0 = min(ip, ult); val i1 = min(ip + 1, ult)
            val fr = (pos - ip).toFloat()
            val s = d[i0] + (d[i1] - d[i0]) * fr
            val y = s * g * env
            l[i] += y
            if (r != null) r[i] += y
            pos += tx * passo
            if (laco) { if (pos >= leA) pos -= (leA - lsA) }
            else if (pos >= fimA) { acabou = true; return }
        }
    }
}

// =====================================================================================
//  Ruidos e pulsos gerados uma vez (dependem da taxa de amostragem)
// =====================================================================================
class Geradas(val sr: Float) {
    private val r = Random(16)
    /** Ruido branco de 2 s (o BUF.ruido do laboratorio). */
    val branco2 = FloatArray((sr * 2).toInt()) { r.nextFloat() * 2 - 1 }
    val vMarrom = ruidoNorm('m')
    val vRosa = ruidoNorm('r')
    val vBranco = ruidoNorm('b')
    val thump = pulso(0.16f, 95f, 42f, 0.032f, 0.55f)
    val v8 = pulso(0.1f, 120f, 55f, 0.02f, 0.5f)
    val dt = pulso(0.06f, 320f, 170f, 0.01f, 0.8f, ringF = 1450f, ringAmt = 0.35f, lpK = 0.4f)
    val pop = pulso(0.05f, 200f, 90f, 0.007f, 1.4f, lpK = 0.6f)
    /** Buzina de duas notas ("ôh-éh"): dois ciclos iguais (o 2o e o laco) + um final curto. */
    val duasNotas = buzinaDuasNotas()

    private fun buzinaDuasNotas(): Amostra {
        val nota = 0.5f                         // segundos por nota
        val grave = 440f; val agudo = 587.33f   // la e re acima: a quarta das buzinas de duas notas
        val ciclo = (2 * nota * sr).toInt()
        val rabo = (0.15f * sr).toInt()
        val n = 2 * ciclo + rabo
        val d = FloatArray(n)
        var fase = 0.0
        var f = grave
        val kf = 1f - exp(-1f / (0.008f * sr))  // a membrana desliza ~8 ms entre as notas
        val bp = Filtro(Filtro.FAIXA, 1400f, 0.9f); bp.ajusta(sr)
        val lp = Filtro(Filtro.PB, 4200f); lp.ajusta(sr)
        for (i in 0 until n) {
            val t = i / sr
            val alvo = if (i >= 2 * ciclo) agudo else if (i % ciclo < ciclo / 2) grave else agudo
            f += (alvo - f) * kf
            val fv = f * (1f + 0.004f * sin(DOIS_PI * 5.5 * t).toFloat())
            fase += fv / sr
            if (fase >= 1.0) fase -= 1.0
            // som de corneta: pulso estreito (rico em harmonicos) e um formante
            var x = if (fase < 0.18) 1f else -0.22f
            x = bp.proc(x) * 1.6f + lp.proc(x) * 0.35f
            var env = min(1f, t / 0.015f)
            if (i >= 2 * ciclo) env *= exp(-(i - 2 * ciclo) / (0.03f * sr))
            d[i] = tanh(x * 1.4f) * 0.75f * env
        }
        val media = d.copyOfRange(ciclo, 2 * ciclo).average().toFloat()        // tira o nivel DC
        for (i in 0 until n) d[i] -= media * min(1f, i / (0.015f * sr)) * (if (i >= 2 * ciclo) exp(-(i - 2 * ciclo) / (0.03f * sr)) else 1f)
        return Amostra(d, sr.toInt())
    }

    private fun ruidoNorm(tipo: Char): FloatArray {
        val n = (sr * 4).toInt()
        val d = FloatArray(n)
        var b0 = 0f; var b1 = 0f; var b2 = 0f; var b3 = 0f; var b4 = 0f; var b5 = 0f; var b6 = 0f; var m = 0f
        var soma = 0.0
        for (i in 0 until n) {
            val w = (r.nextFloat() + r.nextFloat() + r.nextFloat() - 1.5f) * 2
            var y = w
            if (tipo == 'm') { m = 0.995f * m + w; y = m }
            else if (tipo == 'r') {
                b0 = 0.99886f * b0 + w * 0.0555179f; b1 = 0.99332f * b1 + w * 0.0750759f; b2 = 0.96900f * b2 + w * 0.1538520f
                b3 = 0.86650f * b3 + w * 0.3104856f; b4 = 0.55000f * b4 + w * 0.5329522f; b5 = -0.7616f * b5 - w * 0.0168980f
                y = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f; b6 = w * 0.115926f
            }
            d[i] = y; soma += y * y
        }
        val k = (1 / sqrt(soma / n)).toFloat()
        for (i in 0 until n) d[i] *= k
        return d
    }

    // Pulso de explosao: senoide grave com queda de tom + ruido abafado, envelope rapido
    private fun pulso(dur: Float, f0: Float, f1: Float, tau: Float, ruido: Float,
                      ringF: Float = 0f, ringAmt: Float = 0f, ringTau: Float = 0.006f, lpK: Float = 0.15f): Amostra {
        val n = (dur * sr).toInt()
        val d = FloatArray(n)
        var fase = 0.0; var y = 0f; var fr = 0.0
        for (i in 0 until n) {
            val t = i / sr
            val f = f1 + (f0 - f1) * exp(-t / (dur * 0.35f))
            fase += DOIS_PI * f / sr
            y += lpK * ((r.nextFloat() * 2 - 1) - y)
            fr += DOIS_PI * ringF / sr
            val env = min(1f, t / 0.0015f) * exp(-t / tau)
            d[i] = (env * (sin(fase).toFloat() + ruido * y * 2.2f) + ringAmt * sin(fr).toFloat() * exp(-t / ringTau))
        }
        return Amostra(d, sr.toInt())
    }
}

// =====================================================================================
//  Definicoes dos motores (mesmos numeros do laboratorio)
// =====================================================================================
class DefMotor(
    val id: String, val nome: String, val sub: String, val tipo: String, val desc: String,
    val rpm: Boolean = false,                       // true = mostra rpm/marchas; false = %
    val idle: Float = 0f, val max: Float = 100f, val red: Float = 0f,
    val gears: FloatArray = floatArrayOf(), val up: Float = 0f, val upLeve: Float = 0f, val down: Float = 0f,
    val loadRpm: Float = 0f, val cvt: Boolean = false,
    // motores de pulso
    val pattern: FloatArray = floatArrayOf(), val amps: FloatArray = floatArrayOf(), val pulse: String = "",
    val rate: Float = 1f, val jitter: Float = 0f, val drive: Float = 1f,
    val resF: Float = 100f, val resQ: Float = 1f, val resG: Float = 0f, val lpMin: Float = 500f, val lpMax: Float = 4000f,
    val pops: Boolean = false, val gain: Float = 1f,
    // motores gravados
    val banco: List<Pair<String, Float>> = emptyList(), val partidas: List<Pair<String, Float>> = emptyList(), val desliga: String = "",
)

val MOTORES: List<DefMotor> = listOf(
    DefMotor("vess", "Elétrico VESS", "carro elétrico", "vess",
        "Medido na gravação do Hyundai Kona. Parado, um acorde de tons puros; ao sair entram um grave gordo e agudos, todos subindo juntos. Ruído de rolagem cresce com a velocidade."),
    DefMotor("harleyest", "Harley estúdio", "escape, 4 rotações", "banco",
        "Gravações do escapamento a 1090, 1620, 1900 e 4000 rpm, misturadas conforme a rotação. Partida e desligar da mesma moto; estoura soltando o acelerador.",
        rpm = true, idle = 1090f, max = 5500f, red = 4800f, gears = floatArrayOf(150f, 80f, 46f, 35f), up = 3600f, upLeve = 2500f, down = 1500f, loadRpm = 500f,
        banco = listOf("h_e_lenta" to 1090f, "h_e_1600" to 1620f, "h_e_1900" to 1900f, "h_e_4000" to 4000f),
        partidas = listOf("h_e_partida" to 1f), desliga = "h_e_desliga"),
    DefMotor("harleyrec", "Harley gravada", "gravação real", "banco",
        "Uma gravação de marcha lenta (~1330 rpm) em laço, tocada mais rápido conforme a rotação. Marchas longas; às vezes a partida só pega na segunda.",
        rpm = true, idle = 1330f, max = 5000f, red = 4200f, gears = floatArrayOf(120f, 65f, 38f, 29f), up = 3000f, upLeve = 2400f, down = 1700f, loadRpm = 450f,
        banco = listOf("h_lenta" to 1330f), partidas = listOf("h_partida1" to 0.7f, "h_partida2" to 0.3f), desliga = "h_desliga"),
    DefMotor("harley", "Harley V2", "sintetizada, batata-batata", "pulso",
        "Explosões sintetizadas uma a uma, com os intervalos desiguais de 315° e 405° do V2 a 45°.",
        rpm = true, idle = 900f, max = 6000f, red = 5200f, gears = floatArrayOf(260f, 132f, 75f, 58f), up = 4600f, down = 1900f, loadRpm = 700f,
        pattern = floatArrayOf(315f, 405f), amps = floatArrayOf(1f, 0.9f), pulse = "thump", rate = 1f, jitter = 0.07f, drive = 2.4f,
        resF = 110f, resQ = 1.2f, resG = 9f, lpMin = 650f, lpMax = 4200f, pops = true, gain = 0.9f),
    DefMotor("v8", "V8 americano", "muscle car", "pulso",
        "Oito explosões a cada duas voltas, com força diferente entre os cilindros: o borbulhar do V8.",
        rpm = true, idle = 750f, max = 7000f, red = 6200f, gears = floatArrayOf(300f, 160f, 100f, 75f), up = 5800f, down = 2200f, loadRpm = 900f,
        pattern = floatArrayOf(90f, 90f, 90f, 90f, 90f, 90f, 90f, 90f), amps = floatArrayOf(1f, .7f, .92f, .58f, .97f, .78f, .62f, .88f),
        pulse = "v8", rate = 1f, jitter = 0.05f, drive = 2f, resF = 85f, resQ = 1f, resG = 8f, lpMin = 550f, lpMax = 3600f, pops = true, gain = 0.75f),
    DefMotor("doist", "Scooter 2T", "ring-ding-ding", "pulso",
        "Uma explosão a cada volta, rotação alta e CVT: a rotação dispara e a velocidade vem atrás.",
        rpm = true, idle = 1800f, max = 10000f, red = 9000f, cvt = true, loadRpm = 0f,
        pattern = floatArrayOf(360f), amps = floatArrayOf(1f), pulse = "dt", rate = 1f, jitter = 0.08f, drive = 3f,
        resF = 520f, resQ = 3f, resG = 6f, lpMin = 1400f, lpMax = 7500f, pops = false, gain = 0.5f),
    DefMotor("voador", "Carro voador", "desenho animado retrô", "voador",
        "Um tom puro que borbulha: o volume pulsa rápido e a nota balança num vibrato. Acelerar sobe a nota e apressa as pulsadas."),
    DefMotor("tron", "Elétrico Tron", "moto de luz", "tron",
        "Duas dentes de serra quase na mesma nota e uma quadrada uma oitava abaixo, num filtro que abre com a velocidade."),
    DefMotor("jato", "Turbina", "jato decolando", "jato",
        "Ar passando e dois assobios afinados. A turbina tem inércia: demora pra subir e pra cair."),
    DefMotor("nave", "Nave", "hover espacial", "nave",
        "Zumbido grave de duas notas, ar passando e um propulsor que só aparece quando acelera. Com eco."),
)

class Buzina(val id: String, val nome: String, val ls: Double? = null, val le: Double? = null)

val BUZINAS: List<Buzina> = listOf(
    Buzina("b_ahooga", "Ahooga", 0.48104, 1.11233),
    Buzina("b_vintage", "Carro antigo", 0.21792, 0.90462),
    Buzina("b_mustang", "Mustang 1985", 0.17371, 0.48267),
    Buzina("b_mega", "Mega buzina", 0.72958, 2.20471),
    Buzina("b_trem", "Trem", 1.43725, 3.742),
    Buzina("b_bulb", "Fon-fon"),
    Buzina("b_duas", "Ôh-éh", 1.0, 2.0),
    Buzina("b_bici", "Bicicleta"),
)

/** Pedacos [inicio s, duracao s] dentro de h_estouros. */
private val ESTOUROS = Array(20) { doubleArrayOf(it * 0.103, 0.098) }

/** O que os motores recebem a cada passo de controle. */
class EstadoMotor {
    var v = 0f; var a = 0f; var load = 0f; var rpm = 0f; var rpmN = 0f; var dt = 0f
    var estouros = true
}

// =====================================================================================
//  Motores
// =====================================================================================
abstract class Motor(protected val sr: Float, alvoSaida: Float, inicial: Float = 0f) {
    protected val out = Suave(inicial).apply { para(alvoSaida, 0.08f) }
    private var parando = false
    private var tParado = 0f
    var acabou = false
        private set
    protected val ml = FloatArray(BLOCO)
    protected val mr = FloatArray(BLOCO)
    /** Sons que tocam fora do volume do motor (ex.: o de desligar). */
    protected val extras = ArrayList<Voz>()

    abstract fun update(s: EstadoMotor)
    /** Escreve n amostras em ml/mr (sobrescrevendo). */
    protected abstract fun gera(n: Int)

    open fun parar(comSom: Boolean) { parando = true; out.para(0f, 0.06f) }

    fun mistura(l: FloatArray, r: FloatArray, n: Int) {
        if (!(parando && tParado > 0.45f)) {
            gera(n)
            val g0 = out.valor; val g1 = out.avanca(n, sr); val dg = (g1 - g0) / n
            var g = g0
            for (i in 0 until n) { g += dg; l[i] += ml[i] * g; r[i] += mr[i] * g }
        }
        if (parando) tParado += n / sr
        if (extras.isNotEmpty()) {
            for (v in extras) v.soma(l, r, n)
            extras.removeAll { it.acabou }
        }
        if (parando && tParado > 0.45f && extras.isEmpty()) acabou = true
    }
}

// ----- Eletrico VESS (modelo medido no Hyundai Kona) -----
private val VESS_V = floatArrayOf(0f, 1.8f, 6.9f, 12.3f, 21f, 24.3f, 29.8f, 45f)
private class TomVess(val f0: Float, val tab: FloatArray, val marcha: Boolean = false)
private val VESS_TONS = listOf(
    TomVess(98.0f, floatArrayOf(-99f, -99f, -14f, -5f, 6f, 8f, 2f, -6f)),
    TomVess(150.7f, floatArrayOf(-99f, -7f, 4f, 13f, 15f, 15f, -12f, -30f), true),
    TomVess(172.3f, floatArrayOf(-23f, -17f, -20f, -14f, -7f, -3f, -9f, -15f)),
    TomVess(199.0f, floatArrayOf(-99f, -11f, -2f, 2f, 6f, 7f, -5f, -12f)),
    TomVess(218.0f, floatArrayOf(-14f, -18f, -9f, -8f, -4f, -9f, -5f, -12f)),
    TomVess(261.1f, floatArrayOf(-16f, -13f, -12f, -9f, -4f, -3f, -9f, -15f)),
    TomVess(349.9f, floatArrayOf(-3f, -1f, -6f, 5f, 5f, 6f, -11f, -18f)),
    TomVess(395.7f, floatArrayOf(-27f, -28f, -22f, -15f, -8f, -5f, -11f, -18f)),
    TomVess(436.0f, floatArrayOf(-9f, -13f, -10f, -11f, -1f, -6f, -9f, -15f)),
    TomVess(527.6f, floatArrayOf(0f, 2f, 2f, 3f, 3f, 10f, -6f, -12f)),
    TomVess(654.1f, floatArrayOf(-7f, -6f, -4f, -6f, 8f, 1f, -9f, -15f)),
    TomVess(697.1f, floatArrayOf(-11f, -17f, -9f, 3f, 2f, -2f, -13f, -20f)),
    TomVess(786.0f, floatArrayOf(-5f, -4f, -5f, -11f, -6f, -5f, -12f, -18f)),
    TomVess(877.5f, floatArrayOf(-17f, -15f, -6f, -22f, -10f, -10f, -17f, -24f)),
    TomVess(1044.4f, floatArrayOf(-25f, -20f, -10f, -6f, 1f, -1f, -17f, -24f)),
    TomVess(1335.0f, floatArrayOf(-99f, -21f, -17f, -6f, -2f, 2f, -10f, -18f)),
    TomVess(1566.5f, floatArrayOf(-99f, -14f, -14f, -6f, -9f, -6f, -27f, -30f)),
)
private val VESS_MARROM = floatArrayOf(-30f, -18f, -10f, 1f, 5f, 6f, 13f, 15f)
private val VESS_ROSA = floatArrayOf(-43f, -37f, -28f, -27f, -25f, -23f, -13f, -10f)
private val VESS_CHIADO = floatArrayOf(-55f, -46f, -36f, -30f, -27f, -25f, -21f, -19f)
private val VESS_APITO = floatArrayOf(-99f, -36f, -31f, -34f, -46f, -99f, -99f, -99f)
private fun vessInterp(tab: FloatArray, v: Float): Float {
    val x = min(v, VESS_V.last())
    for (i in 1 until VESS_V.size) {
        if (x <= VESS_V[i]) {
            val r = (x - VESS_V[i - 1]) / (VESS_V[i] - VESS_V[i - 1])
            return tab[i - 1] + (tab[i] - tab[i - 1]) * r
        }
    }
    return tab.last()
}

class MotorVess(sr: Float, g: Geradas) : Motor(sr, 0.03f) {
    private class VozTom(val off: Float, val amp: Float, p: Float, sr: Float) {
        val o = Osc('s', sr); val f = Suave(0f); val gl = panL(p) * amp; val gr = panR(p) * amp
    }
    private class Tom(val def: TomVess, val vozes: List<VozTom>) { val g = Suave(0f) }
    private val tons = VESS_TONS.map { t ->
        val vz = listOf(
            VozTom(0f, 1f, 0f, sr),
            VozTom(1.5f + rnd() * 3, 0.55f, -0.75f, sr),
            VozTom(-(1.5f + rnd() * 3), 0.35f, 0.75f, sr),
        )
        vz.forEach { it.f.ja(t.f0 + it.off) }
        Tom(t, vz)
    }
    // ruido estereo: duas copias do mesmo ruido, inicio diferente, uma de cada lado
    private inner class Ruido(buf: FloatArray, filtros: () -> Cadeia, val base: Float, largura: Float) {
        val ini = rnd() * 1.5f
        val l1 = Laco(buf, (ini * sr).toInt()); val l2 = Laco(buf, ((ini + 1.8f + rnd() * 0.5f) * sr).toInt())
        val c1 = filtros(); val c2 = filtros()
        val g1l = panL(-largura) * base; val g1r = panR(-largura) * base
        val g2l = panL(largura) * base; val g2r = panR(largura) * base
        val g = Suave(0f)
    }
    private fun bw6(tipo: Int, f: Float) = arrayOf(Filtro(tipo, f, 0.518f), Filtro(tipo, f, 0.707f), Filtro(tipo, f, 1.932f))
    private val marrom = Ruido(g.vMarrom, { Cadeia(Filtro(Filtro.PB, 1200f, 0.5f)) }, 1.2f, 0.5f)
    private val rosa = Ruido(g.vRosa, { Cadeia(Filtro(Filtro.PA, 120f), Filtro(Filtro.PA, 120f), Filtro(Filtro.PB, 1800f), Filtro(Filtro.PB, 1800f)) }, 3f, 0.9f)
    private val chiado = Ruido(g.vBranco, { Cadeia(*bw6(Filtro.PA, 8300f), *bw6(Filtro.PB, 11000f)) }, 3.4f, 1f)
    private val ruidos = listOf(marrom, rosa, chiado)
    private class Apito(val r: Float, amp: Float, p: Float, sr: Float) { val o = Osc('s', sr); val f = Suave(1300f * r); val gl = panL(p) * amp; val gr = panR(p) * amp }
    private val apitos = listOf(Apito(1f, 1f, -0.5f, sr), Apito(1.12f, 0.7f, 0.5f, sr), Apito(1.26f, 0.5f, 0f, sr))
    private val gApito = Suave(0f)
    private val brilhoL = Filtro(Filtro.AGUDO, 900f); private val brilhoR = Filtro(Filtro.AGUDO, 900f)
    private val brilho = Suave(0f)
    private val lfo = Osc('s', sr)
    private var marchaAtual = 1f

    override fun update(s: EstadoMotor) {
        val v = max(0f, s.v)
        val k = 1 + 0.33f * (min(v, 45f) / 30f).pow(1.5f)
        val vI = if (v <= 8f) v else 8f + (v - 8f) * (19f / 27f)
        val fade = clampf((v - 2f) / 4f, 0f, 1f).pow(2)
        marchaAtual = if (v > 17.6f) 1f / 1.125f else 1f
        for (tn in tons) {
            val f = tn.def.f0 * k
            for (vz in tn.vozes) {
                if (tn.def.marcha) vz.f.para((f + vz.off) * marchaAtual, 0.12f) else vz.f.para(f + vz.off, 0.05f)
            }
            tn.g.para(dbLin(vessInterp(tn.def.tab, vI)), 0.08f)
        }
        marrom.g.para(dbLin(vessInterp(VESS_MARROM, vI)), 0.1f)
        rosa.g.para(fade * dbLin(vessInterp(VESS_ROSA, vI)), 0.1f)
        chiado.g.para(fade * dbLin(vessInterp(VESS_CHIADO, vI)), 0.1f)
        gApito.para(dbLin(vessInterp(VESS_APITO, v)), 0.1f)
        for (a in apitos) a.f.para(min((1300f + 260f * v) * a.r, 15000f), 0.05f)
        brilho.para(6f * s.load, 0.08f)
    }

    override fun gera(n: Int) {
        java.util.Arrays.fill(ml, 0, n, 0f); java.util.Arrays.fill(mr, 0, n, 0f)
        // tons
        for (tn in tons) {
            val g0 = tn.g.valor; val g1 = tn.g.avanca(n, sr)
            for (vz in tn.vozes) vz.f.avanca(n, sr)
            if (g0 < 1e-6f && g1 < 1e-6f) continue
            val dg = (g1 - g0) / n
            for (vz in tn.vozes) {
                var gg = g0; val f = vz.f.valor
                for (i in 0 until n) { gg += dg; val y = vz.o.prox(f) * gg; ml[i] += y * vz.gl; mr[i] += y * vz.gr }
            }
        }
        // ruidos
        for (rd in ruidos) {
            rd.c1.ajusta(sr); rd.c2.ajusta(sr)
            val g0 = rd.g.valor; val g1 = rd.g.avanca(n, sr); val dg = (g1 - g0) / n
            var gg = g0
            for (i in 0 until n) {
                gg += dg
                val a = rd.c1.proc(rd.l1.prox()) * gg
                val b = rd.c2.proc(rd.l2.prox()) * gg
                ml[i] += a * rd.g1l + b * rd.g2l
                mr[i] += a * rd.g1r + b * rd.g2r
            }
        }
        // apito
        val a0 = gApito.valor; val a1 = gApito.avanca(n, sr)
        for (a in apitos) a.f.avanca(n, sr)
        if (a0 > 1e-6f || a1 > 1e-6f) {
            val da = (a1 - a0) / n
            for (a in apitos) {
                var gg = a0; val f = a.f.valor
                for (i in 0 until n) { gg += da; val y = a.o.prox(f) * gg; ml[i] += y * a.gl; mr[i] += y * a.gr }
            }
        }
        // brilho + oscilacao lenta da intensidade (~0,45 Hz, +-10%)
        brilhoL.g = brilho.avanca(n, sr); brilhoR.g = brilhoL.g
        brilhoL.ajusta(sr); brilhoR.ajusta(sr)
        for (i in 0 until n) {
            val m = 1f + 0.1f * lfo.prox(0.45f)
            ml[i] = brilhoL.proc(ml[i]) * m
            mr[i] = brilhoR.proc(mr[i]) * m
        }
    }
}

// ----- Carro voador (desenho retro) -----
class MotorVoador(sr: Float, g: Geradas) : Motor(sr, 0.6f) {
    private val o1 = Osc('s', sr); private val o2 = Osc('t', sr); private val sub = Osc('s', sr)
    private val vib = Osc('s', sr); private val pul = Osc('s', sr)
    private val f = Suave(420f); private val gVib = Suave(10f); private val fVib = Suave(6.5f); private val fPul = Suave(9f)
    private val g2 = Suave(0f); private val gAr = Suave(0f)
    private val lp = Filtro(Filtro.PB, 2500f, 1.2f); private val fLp = Suave(2500f)
    private val nz = Laco(g.branco2, 0); private val bp = Filtro(Filtro.FAIXA, 1800f, 0.8f)
    private val eco = Eco(sr, 0.11f, 0.28f)

    override fun update(s: EstadoMotor) {
        val v = min(s.v, 60f)
        val fr = 380f + v * 14f + s.load * 90f
        f.para(fr, 0.12f)
        gVib.para(fr * 0.025f, 0.1f)
        fVib.para(6f + v * 0.06f, 0.2f)
        fPul.para(8f + v * 0.22f + s.load * 4f, 0.15f)
        g2.para(0.08f + s.load * 0.35f, 0.08f)
        fLp.para(1800f + v * 60f + s.load * 1500f, 0.1f)
        gAr.para(0.01f + s.rpmN * 0.12f, 0.1f)
    }

    override fun gera(n: Int) {
        val fr = f.avanca(n, sr); val dv = gVib.avanca(n, sr); val fv = fVib.avanca(n, sr); val fp = fPul.avanca(n, sr)
        val g20 = g2.valor; val g21 = g2.avanca(n, sr); val ga0 = gAr.valor; val ga1 = gAr.avanca(n, sr)
        lp.f = fLp.avanca(n, sr); lp.ajusta(sr); bp.ajusta(sr)
        for (i in 0 until n) {
            val x = i.toFloat() / n
            val gg2 = g20 + (g21 - g20) * x; val gar = ga0 + (ga1 - ga0) * x
            val desvio = vib.prox(fv) * dv
            val am = 0.55f + 0.45f * pul.prox(fp)
            val s = (o1.prox(fr + desvio) + o2.prox(fr * 1.5f + desvio) * gg2 + sub.prox(fr / 2) * 0.25f) * am
            val y = lp.proc(s)
            val ar = bp.proc(nz.prox()) * gar
            val w = eco.proc(y) * 0.25f
            val o = y + ar + w
            ml[i] = o; mr[i] = o
        }
    }
}

// ----- Eletrico Tron -----
class MotorTron(sr: Float) : Motor(sr, 0.5f) {
    private val o1 = Osc('w', sr); private val o2 = Osc('w', sr); private val o3 = Osc('q', sr); private val wh = Osc('s', sr)
    private val lfo = Osc('s', sr)
    private val fr = Suave(40f); private val f = Filtro(Filtro.PB, 600f, 6f); private val fF = Suave(600f)
    private val mix = Suave(0.3f); private val gWh = Suave(0.03f)

    override fun update(s: EstadoMotor) {
        val x = 32f + s.v * 5f + s.load * 30f
        fr.para(x, 0.05f)
        fF.para(250f + x * 5f + s.load * 3000f, 0.05f)
        mix.para(0.2f + 0.15f * s.load + 0.1f * s.rpmN, 0.05f)
        gWh.para(0.01f + 0.04f * s.rpmN, 0.05f)
    }

    override fun gera(n: Int) {
        val x = fr.avanca(n, sr)
        f.f = fF.avanca(n, sr); f.ajusta(sr)
        val m0 = mix.valor; val m1 = mix.avanca(n, sr); val w0 = gWh.valor; val w1 = gWh.avanca(n, sr)
        for (i in 0 until n) {
            val k = i.toFloat() / n
            val d = lfo.prox(5.5f) * 1.2f
            val s = f.proc(o1.prox(x + d) + o2.prox(x * 1.007f + d) + o3.prox(x / 2) * 0.5f) * (m0 + (m1 - m0) * k) +
                    wh.prox(x * 6f) * (w0 + (w1 - w0) * k)
            ml[i] = s; mr[i] = s
        }
    }
}

// ----- Turbina -----
class MotorJato(sr: Float, g: Geradas) : Motor(sr, 0.8f) {
    private val n1 = Laco(g.branco2, 0); private val n2 = Laco(g.branco2, (0.7f * sr).toInt())
    private val bp = Filtro(Filtro.FAIXA, 400f, 0.9f); private val lp = Filtro(Filtro.PB, 170f)
    private val w1 = Osc('s', sr); private val w2 = Osc('s', sr)
    private val fBp = Suave(400f); private val gN = Suave(0.2f); private val gR = Suave(0.3f); private val fW = Suave(900f); private val gW = Suave(0.03f)
    private var spool = 0.12f

    override fun update(s: EstadoMotor) {
        val alvo = 0.12f + 0.88f * min(1f, s.rpmN * 0.8f + s.load * 0.45f)
        spool += (alvo - spool) * min(1f, s.dt * (if (alvo > spool) 0.9f else 0.55f))
        fBp.para(250f + spool * 2600f, 0.05f)
        gN.para(0.12f + spool * 0.55f, 0.05f)
        gR.para(0.18f + spool * 0.55f, 0.05f)
        fW.para(700f + spool * 3600f, 0.05f)
        gW.para(0.015f + spool * 0.05f, 0.05f)
    }

    override fun gera(n: Int) {
        bp.f = fBp.avanca(n, sr); bp.ajusta(sr); lp.ajusta(sr)
        val fw = fW.avanca(n, sr)
        val a0 = gN.valor; val a1 = gN.avanca(n, sr); val b0 = gR.valor; val b1 = gR.avanca(n, sr); val c0 = gW.valor; val c1 = gW.avanca(n, sr)
        for (i in 0 until n) {
            val k = i.toFloat() / n
            val s = bp.proc(n1.prox()) * (a0 + (a1 - a0) * k) + lp.proc(n2.prox()) * (b0 + (b1 - b0) * k) +
                    (w1.prox(fw) + w2.prox(fw * 1.51f)) * (c0 + (c1 - c0) * k)
            ml[i] = s; mr[i] = s
        }
    }
}

// ----- Nave -----
class MotorNave(sr: Float, g: Geradas) : Motor(sr, 0.7f) {
    private val d1 = Osc('s', sr); private val d2 = Osc('s', sr); private val lfo = Osc('s', sr)
    private val nz = Laco(g.branco2, 0); private val bp = Filtro(Filtro.FAIXA, 300f, 2f)
    private val th = Osc('w', sr); private val lpT = Filtro(Filtro.PB, 600f, 3f)
    private val fBp = Suave(300f); private val gAr = Suave(0.05f); private val fTh = Suave(70f); private val fLp = Suave(600f); private val gT = Suave(0f); private val fD2 = Suave(82.8f)
    private val eco = Eco(sr, 0.31f, 0.4f)

    override fun update(s: EstadoMotor) {
        fBp.para(200f + s.v * 45f, 0.08f)
        gAr.para(0.04f + s.rpmN * 0.35f, 0.08f)
        fTh.para(70f * (1 + s.load * 1.2f + s.rpmN * 0.5f), 0.06f)
        fLp.para(300f + s.load * 1500f, 0.06f)
        gT.para(s.load * 0.3f, 0.06f)
        fD2.para(82.8f * (1 + s.rpmN * 0.05f), 0.1f)
    }

    override fun gera(n: Int) {
        bp.f = fBp.avanca(n, sr); bp.ajusta(sr)
        lpT.f = fLp.avanca(n, sr); lpT.ajusta(sr)
        val ft = fTh.avanca(n, sr); val f2 = fD2.avanca(n, sr)
        val a0 = gAr.valor; val a1 = gAr.avanca(n, sr); val t0 = gT.valor; val t1 = gT.avanca(n, sr)
        for (i in 0 until n) {
            val k = i.toFloat() / n
            val gD = 0.25f + 0.08f * lfo.prox(0.25f)
            val mix = (d1.prox(55f) + d2.prox(f2)) * gD + bp.proc(nz.prox()) * (a0 + (a1 - a0) * k) +
                      lpT.proc(th.prox(ft)) * (t0 + (t1 - t0) * k)
            val o = mix + eco.proc(mix) * 0.35f
            ml[i] = o; mr[i] = o
        }
    }
}

// ----- Explosoes sintetizadas (Harley V2, V8, 2T) -----
class MotorPulso(sr: Float, private val e: DefMotor, g: Geradas) : Motor(sr, e.gain) {
    private val buf = when (e.pulse) { "v8" -> g.v8; "dt" -> g.dt; else -> g.thump }
    private val pop = g.pop
    private val vozes = ArrayList<Voz>()
    private val bus = FloatArray(BLOCO)
    private val tk = tanh(e.drive)
    private val res = Filtro(Filtro.PICO, e.resF, e.resQ, e.resG)
    private val lp = Filtro(Filtro.PB, 2000f); private val fLp = Suave(2000f)
    private var t = 0.0                  // relogio do motor (s)
    private var prox = 0.05
    private var i = 0

    override fun update(s: EstadoMotor) {
        fLp.para(e.lpMin + (e.lpMax - e.lpMin) * (0.35f * s.rpmN + 0.65f * s.load), 0.04f)
        if (prox < t) prox = t + 0.01
        while (prox < t + 0.12) {
            disparo(prox, s)
            prox += e.pattern[i % e.pattern.size] / (max(s.rpm, 200f) * 6.0)
            i++
        }
    }

    private fun disparo(quando: Double, s: EstadoMotor) {
        val v = Voz(buf, sr)
        v.taxa.ja(e.rate * (0.85f + 0.45f * s.rpmN) * (1 + (rnd() - 0.5f) * e.jitter))
        v.ganho.ja(e.amps[i % e.amps.size] * (0.3f + 0.7f * s.load + 0.15f * s.rpmN) * (1 + (rnd() - 0.5f) * 0.25f))
        v.atraso = ((quando - t) * sr).toInt()
        vozes.add(v)
        if (e.pops && s.a < -2.5f && s.load < 0.05f && s.rpm > e.idle * 1.8f && rnd() < 0.05f) {
            val p = Voz(pop, sr)
            p.taxa.ja(0.8f + rnd() * 0.5f)
            p.ganho.ja(0.8f + rnd() * 0.6f)
            p.atraso = ((quando + 0.004 - t) * sr).toInt()
            vozes.add(p)
        }
    }

    override fun gera(n: Int) {
        java.util.Arrays.fill(bus, 0, n, 0f)
        for (v in vozes) v.soma(bus, null, n)
        vozes.removeAll { it.acabou }
        lp.f = fLp.avanca(n, sr); lp.ajusta(sr); res.ajusta(sr)
        for (k in 0 until n) {
            val x = clampf(bus[k] * 0.6f, -1f, 1f)
            val y = lp.proc(res.proc(tanh(e.drive * x) / tk))
            ml[k] = y; mr[k] = y
        }
        t += n / sr.toDouble()
    }
}

// ----- Harleys gravadas: banco de lacos em rotacoes diferentes + partida/desliga + estouros -----
class MotorBanco(sr: Float, private val e: DefMotor, private val sons: Map<String, Amostra>, ligando: Boolean) :
    Motor(sr, 0.85f, inicial = 0.85f) {
    private class VozBanco(val voz: Voz, val rpm: Float)
    private val vozes = ArrayList<VozBanco>()
    private var partida: Voz? = null
    private val lp = Filtro(Filtro.PB, 6000f); private val fLp = Suave(6000f)
    private val lacos = FloatArray(BLOCO)
    private val estourosV = ArrayList<Voz>()
    private var cargaAnt = 0f
    private var rajada = 0

    init {
        var inicio = 0.02
        if (ligando && e.partidas.isNotEmpty()) {
            var r = rnd(); var nome = e.partidas[0].first
            for ((nm, p) in e.partidas) { if (r < p) { nome = nm; break }; r -= p }
            sons[nome]?.let { a ->
                val v = Voz(a, sr); v.atraso = (0.02 * sr).toInt()
                partida = v
                inicio = 0.02 + a.dur - 0.01
            }
        }
        e.banco.forEachIndexed { i, (id, rpm) ->
            val a = sons[id] ?: return@forEachIndexed
            val v = Voz(a, sr)
            v.laco = true
            v.atraso = (inicio * sr).toInt()
            v.entrada = (0.02 * sr).toInt()
            v.ganho.ja(if (i == 0) 1f else 0f)
            vozes.add(VozBanco(v, rpm))
        }
    }

    override fun update(s: EstadoMotor) {
        if (vozes.isEmpty()) return
        val R = max(s.rpm, 300f); val n = vozes.size
        var k = 0
        while (k < n - 2 && R > vozes[k + 1].rpm) k++
        vozes.forEachIndexed { i, v ->
            var w = 0f
            if (n == 1 || R <= vozes[0].rpm) w = if (i == 0) 1f else 0f
            else if (R >= vozes[n - 1].rpm) w = if (i == n - 1) 1f else 0f
            else {
                val x = ((ln(R) - ln(vozes[k].rpm)) / (ln(vozes[k + 1].rpm) - ln(vozes[k].rpm)))
                if (i == k) w = cos(x * PI / 2).toFloat() else if (i == k + 1) w = sin(x * PI / 2).toFloat()
            }
            v.voz.ganho.para(w, 0.05f)
            v.voz.taxa.para(clampf(R / v.rpm, 0.6f, 2.6f), 0.03f)
        }
        fLp.para(5000f + (0.4f * s.rpmN + 0.6f * s.load) * 9000f, 0.05f)
        out.para(0.85f + 0.3f * s.load, 0.05f)
        // estouros: acelerador fechado com a rotacao acima da lenta (desacelerando / freando)
        if (s.estouros) {
            if (cargaAnt > 0.45f && s.load < 0.12f) rajada = 2 + (rnd() * 3).toInt()
            cargaAnt = s.load
            val acima = clampf((s.rpm - e.idle * 1.15f) / 1000f, 0f, 1f)
            val desacel = s.load < 0.1f && s.a < -0.8f && acima > 0f
            if (desacel || rajada > 0) {
                val taxa = (2f + min(8f, -s.a * 0.8f)) * max(acima, if (rajada > 0) 0.6f else 0f)
                if (rnd() < taxa * s.dt) {
                    estouro(0.45f + rnd() * 0.6f)
                    if (rajada > 0) rajada--
                }
            }
        }
    }

    private fun estouro(amp: Float) {
        val a = sons["h_estouros"] ?: return
        val (ini, dur) = ESTOUROS[(rnd() * ESTOUROS.size).toInt().coerceAtMost(ESTOUROS.size - 1)].let { it[0] to it[1] }
        val v = Voz(a, sr)
        v.taxa.ja(0.85f + rnd() * 0.3f)
        v.ganho.ja(amp)
        v.comecaEm(ini); v.ateSeg(ini + dur)
        v.atraso = (rnd() * 0.015f * sr).toInt()
        estourosV.add(v)
    }

    override fun parar(comSom: Boolean) {
        super.parar(comSom)
        out.para(0f, if (comSom) 0.03f else 0.06f)
        if (comSom) sons[e.desliga]?.let { extras.add(Voz(it, sr)) }
    }

    override fun gera(n: Int) {
        java.util.Arrays.fill(lacos, 0, n, 0f)
        java.util.Arrays.fill(ml, 0, n, 0f)
        for (v in vozes) v.voz.soma(lacos, null, n)
        lp.f = fLp.avanca(n, sr); lp.ajusta(sr)
        for (i in 0 until n) ml[i] = lp.proc(lacos[i])
        partida?.let { it.soma(ml, null, n); if (it.acabou) partida = null }
        for (v in estourosV) v.soma(ml, null, n)
        estourosV.removeAll { it.acabou }
        System.arraycopy(ml, 0, mr, 0, n)
    }
}

// =====================================================================================
//  Compressor (parecido com o DynamicsCompressor do Web Audio: -14 dB, 4:1, joelho 30 dB)
// =====================================================================================
class Compressor(private val sr: Float) {
    private val thr = -14f; private val ratio = 4f; private val knee = 30f
    private val makeup = 1.47f
    private val ka = 1f - exp(-1f / (0.003f * sr))
    private val kr = 1f - exp(-1f / (0.25f * sr))
    private var env = 0f    // reducao atual (dB, <= 0)
    fun ganho(pico: Float): Float {
        val x = if (pico < 1e-6f) -120f else 20f * log10(pico)
        val gr = when {
            x < thr -> 0f
            x < thr + knee -> (1f / ratio - 1f) * (x - thr) * (x - thr) / (2f * knee)
            else -> (1f / ratio - 1f) * (x - thr - knee / 2f)
        }
        env += (gr - env) * (if (gr < env) ka else kr)
        return dbLin(env + makeup)
    }
}

// =====================================================================================
//  Pneu cantando nas freadas fortes (vale para todos os motores)
// =====================================================================================
class Pneu(private val sr: Float, g: Geradas) {
    private val o = Osc('w', sr); private val o2 = Osc('s', sr); private val vib = Osc('s', sr)
    private val bp = Filtro(Filtro.FAIXA, 2400f, 6f)
    private val nz = Laco(g.branco2, (0.3f * sr).toInt()); private val bpN = Filtro(Filtro.FAIXA, 2800f, 2f)
    private val f = Suave(1250f)
    private val ganho = Suave(0f)
    private var deriva = 0f
    var ativo = true

    fun update(v: Float, a: Float, dt: Float) {
        val forca = clampf((-a - 9f) / 10f, 0f, 1f) * clampf((v - 6f) / 8f, 0f, 1f)
        val alvo = if (ativo) forca * 0.35f else 0f
        ganho.para(alvo, if (alvo > ganho.valor) 0.04f else 0.12f)
        deriva += ((rnd() - 0.5f) * 900f - deriva) * min(1f, dt * 3f)        // o tom passeia
        f.para(clampf(950f + v * 12f + deriva * 0.3f, 700f, 2200f), 0.05f)
    }

    fun soma(l: FloatArray, r: FloatArray, n: Int) {
        val g0 = ganho.valor; val g1 = ganho.avanca(n, sr)
        val fr = f.avanca(n, sr)
        if (g0 < 1e-5f && g1 < 1e-5f) return
        bp.f = fr * 2f; bp.ajusta(sr); bpN.ajusta(sr)
        val dg = (g1 - g0) / n; var g = g0
        for (i in 0 until n) {
            g += dg
            val ff = fr * (1f + 0.025f * vib.prox(7f))
            val y = (bp.proc(o.prox(ff)) * 1.5f + o2.prox(ff) * 0.5f + bpN.proc(nz.prox()) * 0.3f) * g
            l[i] += y; r[i] += y
        }
    }
}

// =====================================================================================
//  A mesa: simulacao + motor + buzinas + cadeia final
// =====================================================================================
class Mesa(val sr: Float, sonsGravados: Map<String, Amostra>) {
    private val geradas = Geradas(sr)
    private val sons: Map<String, Amostra> = sonsGravados + ("b_duas" to geradas.duasNotas)
    private val pneu = Pneu(sr, geradas)
    private val cmds = ConcurrentLinkedQueue<(Mesa) -> Unit>()
    /** Executa no proximo bloco, na thread do audio. */
    fun manda(c: (Mesa) -> Unit) { cmds.add(c) }

    // ajustes (mexidos so pela thread do audio, via manda)
    var volume = 0.7f
    var timbre = 0f
    var reacao = 1f
    var marchas = true
    var estouros = true
    var pneuCantando: Boolean
        get() = pneu.ativo
        set(x) { pneu.ativo = x }

    // simulacao (para testar sem a roda)
    var alvo = 0f
    var auto = false
    var segAcel = false
    var segFreio = false
    private var autoT = 0f

    // estado da simulacao
    var v = 0f; private set
    private var vAnt = 0f
    var a = 0f; private set
    var load = 0f; private set
    var rpm = 0f; private set
    var marcha = 0; private set
    private var corte = 0f

    var motorId = MOTORES[0].id; private set
    var ligado = false; private set
    private var motor: Motor? = null
    private val saindo = ArrayList<Motor>()
    private val buzinas = ArrayList<Voz>()
    private var buzSegura: Voz? = null
    private var buzSeguraDef: Buzina? = null
    private var buzSeguraT = 0.0       // tempo desde que comecou (s)

    private val es = EstadoMotor()
    private val bl = FloatArray(BLOCO); private val br = FloatArray(BLOCO)
    private val vol = Suave(volume)
    private val graveL = Filtro(Filtro.GRAVE, 250f); private val graveR = Filtro(Filtro.GRAVE, 250f)
    private val agudoL = Filtro(Filtro.AGUDO, 2200f); private val agudoR = Filtro(Filtro.AGUDO, 2200f)
    private val comp = Compressor(sr)
    private var fase = 0

    val def: DefMotor get() = MOTORES.first { it.id == motorId }
    /** true enquanto ha algo tocando (o Som.kt desliga o audio quando fica false por um tempo). */
    val tocando: Boolean get() = ligado || saindo.isNotEmpty() || buzinas.isNotEmpty() || cmds.isNotEmpty()

    private fun cria(d: DefMotor, ligando: Boolean): Motor = when (d.tipo) {
        "vess" -> MotorVess(sr, geradas)
        "banco" -> MotorBanco(sr, d, sons, ligando)
        "pulso" -> MotorPulso(sr, d, geradas)
        "voador" -> MotorVoador(sr, geradas)
        "tron" -> MotorTron(sr)
        "jato" -> MotorJato(sr, geradas)
        else -> MotorNave(sr, geradas)
    }

    fun ligar() {
        if (ligado) return
        ligado = true
        val d = def
        rpm = if (d.rpm) d.idle else 0f; marcha = 0
        motor = cria(d, true)
    }

    fun desligar() {
        if (!ligado) return
        ligado = false
        motor?.let { it.parar(true); saindo.add(it) }
        motor = null
    }

    fun escolher(id: String) {
        if (MOTORES.none { it.id == id }) return
        motorId = id
        val d = def
        rpm = if (d.rpm) d.idle else 0f; marcha = 0
        if (ligado) {
            motor?.let { it.parar(false); saindo.add(it) }
            motor = cria(d, false)
        }
    }

    /** Aperta uma buzina; as que tem laco tocam ate soltar. */
    fun apertaBuzina(id: String) {
        val b = BUZINAS.firstOrNull { it.id == id } ?: return
        val a = sons[id] ?: return
        soltaBuzina()
        val v = Voz(a, sr)
        if (b.ls != null && b.le != null) v.lacoEm(b.ls, b.le)
        buzinas.add(v)
        if (b.ls != null) { buzSegura = v; buzSeguraDef = b; buzSeguraT = 0.0 }
    }

    fun soltaBuzina() {
        val v = buzSegura ?: return
        val b = buzSeguraDef ?: return
        buzSegura = null; buzSeguraDef = null
        val a = sons[b.id] ?: return
        val espera = max(0.0, b.ls!! - buzSeguraT)                // pelo menos o ataque inteiro
        val esp = (espera * sr).toInt()
        val x = (0.025 * sr).toInt()
        // o rabo, a partir do fim do laco
        val rabo = Voz(a, sr)
        rabo.comecaEm(b.le!!); rabo.atraso = esp; rabo.entrada = x
        buzinas.add(rabo)
        v.sai(esp, x)
    }

    // ---------------- simulacao (passo + calcRpm do laboratorio) ----------------
    // amostras da roda: chegam em degraus (alguns por segundo); entre uma e outra, segue a
    // tendencia das duas ultimas (sem isso o som fica atrasado ou em degraus)
    private var rodaQuando = 0L
    private var rodaV = 0f
    private var rodaInc = 0f        // km/h por segundo entre as duas ultimas amostras
    private var rodaIntervalo = 0.2f
    private var desdeAmostra = 0f

    private fun passo(dt: Float, vRoda: Float, corrente: Float, quando: Long) {
        if (!vRoda.isNaN()) {
            val vr = abs(vRoda)
            if (quando != rodaQuando) {
                if (rodaQuando != 0L) {
                    val iv = clampf((quando - rodaQuando) / 1000f, 0.03f, 1f)
                    rodaIntervalo += (iv - rodaIntervalo) * 0.3f
                    rodaInc += (clampf((vr - rodaV) / iv, -40f, 40f) - rodaInc) * 0.6f
                }
                rodaQuando = quando; rodaV = vr; desdeAmostra = 0f
            }
            desdeAmostra += dt
            val previsto = max(0f, rodaV + rodaInc * min(desdeAmostra, rodaIntervalo * 1.2f))
            v += (previsto - v) * min(1f, dt / 0.04f)
        } else {
            rodaQuando = 0L
            if (segAcel) alvo = min(50f, alvo + 14f * dt)
            if (segFreio) alvo = max(0f, alvo - 22f * dt)
            if (auto && !segAcel && !segFreio) {
                autoT -= dt
                if (autoT <= 0f) { alvo = if (rnd() < 0.2f) 0f else (8 + rnd() * 40).toInt().toFloat(); autoT = 3 + rnd() * 5 }
            }
            val diff = alvo - v
            val taxa = if (diff > 0) 7f else if (segFreio) 18f else 10f   // "Frear" segurado = freada forte
            v += Math.signum(diff) * min(abs(diff), taxa * dt * min(1f, abs(diff) / 4f + 0.15f))
        }
        if (dt > 0) a += ((v - vAnt) / dt - a) * min(1f, dt * 6f)
        vAnt = v
        var la = clampf(a / (6f / reacao), 0f, 1f)
        if (v > 2f && a > -0.5f) la = max(la, 0.18f)
        if (!corrente.isNaN() && corrente > 0f) la = max(la, clampf(corrente / 30f, 0f, 1f))
        if (corte > 0f) { la = 0f; corte -= dt }
        load += (la - load) * min(1f, dt * 8f)
        calcRpm(def, dt)
    }

    private fun calcRpm(e: DefMotor, dt: Float) {
        var alvoR: Float
        if (e.rpm) {
            if (e.cvt) {
                alvoR = if (v < 1f && load < 0.05f) e.idle else e.idle + 1200f + v * 85f + load * 3800f
            } else if (!marchas) {
                alvoR = e.idle + v * (e.up - e.idle) / 50f + load * e.loadRpm
            } else {
                if (marcha >= e.gears.size) marcha = e.gears.size - 1
                val r = e.idle + v * e.gears[marcha]
                val up = if (e.upLeve > 0f && load < 0.3f) e.upLeve else e.up
                if (r > up && marcha < e.gears.size - 1) { marcha++; corte = 0.14f }
                else if (r < e.down && marcha > 0) marcha--
                alvoR = e.idle + v * e.gears[marcha] + load * e.loadRpm * (if (v < 8f) 1.6f else 1f)
            }
            alvoR = min(alvoR, e.max)
            rpm += (alvoR - rpm) * min(1f, dt * (if (alvoR > rpm) 6f else 4f))
        } else {
            alvoR = min(100f, (v / 50f) * 75f + load * 25f)
            rpm += (alvoR - rpm) * min(1f, dt * 5f)
        }
    }

    /**
     * Gera n quadros estereo intercalados em out (L R L R ...).
     * vRoda: velocidade da roda em km/h, ou NaN para usar a simulacao. corrente: A, ou NaN.
     * quando: hora (ms) em que chegou a amostra da roda (para saber quando chega uma nova).
     */
    fun gera(out: FloatArray, n: Int, vRoda: Float, corrente: Float, quando: Long = 0L) {
        var feito = 0
        while (feito < n) {
            if (fase == 0) {
                while (true) { val c = cmds.poll() ?: break; c(this) }
                val dt = BLOCO * CONTROLE / sr
                passo(dt, vRoda, corrente, quando)
                val d = def
                es.v = v; es.a = a; es.load = load; es.rpm = rpm; es.rpmN = rpm / d.max; es.dt = dt; es.estouros = estouros
                motor?.update(es)
                for (m in saindo) m.update(es)
                pneu.update(v, a, dt)
                vol.para(volume, 0.03f)
                graveL.g = -timbre * 0.7f; graveR.g = graveL.g
                agudoL.g = timbre; agudoR.g = timbre
            }
            fase = (fase + 1) % CONTROLE
            val k = min(BLOCO, n - feito)
            java.util.Arrays.fill(bl, 0f); java.util.Arrays.fill(br, 0f)
            motor?.mistura(bl, br, k)
            for (m in saindo) m.mistura(bl, br, k)
            saindo.removeAll { it.acabou }
            pneu.soma(bl, br, k)
            for (b in buzinas) b.soma(bl, br, k)
            buzinas.removeAll { it.acabou }
            buzSeguraT += k / sr.toDouble()
            graveL.ajusta(sr); graveR.ajusta(sr); agudoL.ajusta(sr); agudoR.ajusta(sr)
            val g0 = vol.valor; val g1 = vol.avanca(k, sr); val dg = (g1 - g0) / k
            var g = g0
            for (i in 0 until k) {
                g += dg
                val l = agudoL.proc(graveL.proc(bl[i] * g))
                val r = agudoR.proc(graveR.proc(br[i] * g))
                val c = comp.ganho(max(abs(l), abs(r)))
                out[(feito + i) * 2] = limita(l * c)
                out[(feito + i) * 2 + 1] = limita(r * c)
            }
            feito += k
        }
    }
}
