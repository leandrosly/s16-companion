package systems.leandro.s16

// Telas do app (Jetpack Compose): conexao com o C3 no topo e quatro paginas
// (Roda, Piscas, Fitas, Buzina) que trocam tocando nos botoes ou arrastando para o lado.

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.launch
import kotlin.math.roundToInt

// ---------------- cores ----------------
private val Laranja = Color(0xFFF2A23A)
private val Fundo = Color(0xFF14171B)
private val Painel = Color(0xFF1D2126)
private val Texto = Color(0xFFE5E8EC)
private val Apagado = Color(0xFF8D97A3)
private val Vermelho = Color(0xFFFF5B4B)

private val esquema = darkColorScheme(
    primary = Laranja, onPrimary = Color(0xFF1B1200),
    secondary = Color(0xFF4FC3CC),
    background = Fundo, onBackground = Texto,
    surface = Painel, onSurface = Texto,
)

// mesma paleta do C3 (LED_PAL no ponte_c3.ino), na mesma ordem
private val PALETA = listOf(
    Color(255, 0, 0), Color(255, 90, 0), Color(255, 200, 0), Color(0, 255, 0),
    Color(0, 255, 200), Color(0, 0, 255), Color(0x4E, 0, 0x8F), Color(200, 200, 200),
)
private val NOMES_COR = listOf("vermelho", "laranja", "amarelo", "verde", "ciano", "azul", "violeta", "branco")
private val MODOS_FITA = listOf("Apagadas", "Cor fixa", "Respirando", "Arco-íris", "Automático")
private val LUZES = listOf("Apagadas", "Laranja", "Vermelhas", "Piscando")

// buzina externa: mesmos codigos do display (BX_CMD)
private val BUZINA = listOf(
    "Volume −" to 0xCC, "Volume +" to 0x99,
    "◀ Som" to 0x66, "Som ▶" to 0xFF,
    "Alarme liga" to 0x55, "Alarme desliga" to 0x33,
)

// ---------------- estrutura ----------------
@Composable
fun App(
    link: C3Link,
    macSalvo: String?,
    temPermissao: Boolean,
    btLigado: Boolean,
    pedirPermissao: () -> Unit,
    salvarMac: (String) -> Unit,
) {
    MaterialTheme(colorScheme = esquema) {
        Surface(Modifier.fillMaxSize(), color = Fundo) {
            Column(
                Modifier.fillMaxSize().safeDrawingPadding().padding(horizontal = 16.dp, vertical = 12.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Text("S16 Companion", fontSize = 24.sp, fontWeight = FontWeight.Bold, color = Texto)
                when {
                    !temPermissao -> Aviso("O app precisa da permissão de Bluetooth para achar o C3 na roda.", "Permitir", pedirPermissao)
                    !btLigado -> Aviso("O Bluetooth está desligado. Ligue e volte para o app.", null) {}
                    else -> Principal(link, macSalvo, salvarMac)   // (dentro da Column: ocupa o resto da tela)
                }
            }
        }
    }
}

@Composable
private fun ColumnScope.Principal(link: C3Link, macSalvo: String?, salvarMac: (String) -> Unit) {
    val con by link.conexao.collectAsState()
    val est by link.estado.collectAsState()

    CartaoConexao(link, con, est, macSalvo, salvarMac)

    if ((con == Conexao.CONECTADO && est.recebido) || con == Conexao.DEMO) {
        val paginas = listOf("Roda", "Piscas", "Fitas", "Buzina")
        val pager = rememberPagerState(pageCount = { paginas.size })
        val escopo = rememberCoroutineScope()
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            paginas.forEachIndexed { i, nome ->
                Botao(nome, Modifier.weight(1f), selecionado = pager.currentPage == i) {
                    escopo.launch { pager.animateScrollToPage(i) }
                }
            }
        }
        HorizontalPager(
            state = pager,
            modifier = Modifier.fillMaxWidth().weight(1f),
            pageSpacing = 16.dp,
            verticalAlignment = Alignment.Top,
        ) { pagina ->
            Column(
                Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                when (pagina) {
                    0 -> TelaRoda(link)
                    1 -> TelaPiscas(link, est)
                    2 -> TelaFitas(link, est)
                    else -> TelaBuzina(link)
                }
            }
        }
    }
}

// ---------------- conexao ----------------
@Composable
private fun CartaoConexao(link: C3Link, con: Conexao, est: EstadoC3, macSalvo: String?, salvarMac: (String) -> Unit) {
    val achados by link.achados.collectAsState()
    Secao("C3 na roda") {
        val texto = when (con) {
            Conexao.DESCONECTADO -> if (macSalvo == null) "Nenhum C3 escolhido ainda." else "Desconectado ($macSalvo)"
            Conexao.PROCURANDO -> "Procurando..."
            Conexao.CONECTANDO -> "Conectando em $macSalvo..."
            Conexao.CONECTADO ->
                if (!est.recebido) "Conectado, esperando o C3 responder..."
                else "Conectado · rádio 433 ${if (est.radioOk) "ok" else "com problema"} · apps no C3: ${est.apps}"
            Conexao.DEMO -> "Demonstração: sem C3, nada é enviado de verdade."
        }
        Text(texto, color = Texto)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            if (con == Conexao.DEMO) {
                OutlinedButton(onClick = { link.sairDemo() }) { Text("Sair da demonstração") }
            } else if (con == Conexao.CONECTADO || con == Conexao.CONECTANDO) {
                OutlinedButton(onClick = { link.desconectar() }) { Text("Desconectar") }
            } else {
                if (macSalvo != null) Button(onClick = { link.conectar(macSalvo) }) { Text("Conectar") }
                OutlinedButton(onClick = { link.procurar() }) {
                    Text(if (macSalvo == null) "Procurar o C3" else "Procurar outro")
                }
            }
        }
        if (con == Conexao.DESCONECTADO) {
            Text(
                "Ver as telas sem o C3 (demonstração)",
                Modifier.clickable { link.entrarDemo() }.padding(vertical = 4.dp),
                color = Laranja, fontSize = 14.sp,
            )
        }
        if (con == Conexao.PROCURANDO || (con == Conexao.DESCONECTADO && achados.isNotEmpty())) {
            if (achados.isEmpty()) {
                Text("Ligue a roda: o C3 aparece com o nome dela (KS-...).", color = Apagado, fontSize = 13.sp)
            }
            achados.forEach { a ->
                val ehRoda = a.mac.equals(MAC_RODA, ignoreCase = true)
                Row(
                    Modifier.fillMaxWidth()
                        .clip(RoundedCornerShape(8.dp))
                        .background(Fundo)
                        .clickable(enabled = !ehRoda) { salvarMac(a.mac); link.conectar(a.mac) }
                        .padding(12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(Modifier.weight(1f)) {
                        Text(a.nome, color = if (ehRoda) Apagado else Texto)
                        Text(
                            a.mac + if (ehRoda) "  ·  a roda em si, não o C3" else "",
                            fontSize = 12.sp, color = Apagado,
                        )
                    }
                    Text("${a.rssi} dBm", fontSize = 12.sp, color = Apagado)
                }
            }
        }
    }
}

// ---------------- dados e comandos da roda ----------------
@Composable
private fun TelaRoda(link: C3Link) {
    val r by link.roda.collectAsState()
    val temDados = r.quando != 0L && System.currentTimeMillis() - r.quando < 3_000
    Secao("Roda") {
        if (!temDados) {
            Text("Sem dados da roda ainda.", color = Apagado)
        } else {
            Row(verticalAlignment = Alignment.Bottom) {
                Text("%.1f".format(r.velocidade), fontSize = 56.sp, fontWeight = FontWeight.Bold, color = Texto)
                Text("  km/h", fontSize = 18.sp, color = Apagado, modifier = Modifier.padding(bottom = 10.dp))
            }
            val corBat = when {
                r.bateria > 40 -> Texto
                r.bateria > 15 -> Laranja
                else -> Vermelho
            }
            val dados = listOf(
                // (o "%" da bateria fica FORA do format: dentro dele, "% ·" e lido como codigo de formatacao)
                Triple("Bateria", "${r.bateria}%" + (if (r.bateriaPeloBms) "" else " (≈)") + " · " + "%.1f V".format(r.tensao), corBat),
                Triple("Corrente", "%.1f A".format(r.corrente), Texto),
                Triple("Temperatura", "%.0f °C".format(r.temperatura), Texto),
                Triple("PWM", if (r.pwm >= 0) "${r.pwm}%" else "—", if (r.pwm >= 80) Vermelho else Texto),
                Triple("Viagem", "%.1f km".format(r.tripKm), Texto),
                Triple("Máxima", "%.1f km/h".format(r.maxima), Texto),
            )
            dados.chunked(2).forEach { par ->
                Row(Modifier.fillMaxWidth()) {
                    par.forEach { (rotulo, valor, cor) ->
                        Column(Modifier.weight(1f)) {
                            Text(rotulo, fontSize = 12.sp, color = Apagado)
                            Text(valor, fontSize = 18.sp, color = cor, fontWeight = FontWeight.SemiBold)
                        }
                    }
                }
            }
        }
    }
    Secao("Comandos da roda") {
        BotaoGrande("Buzina da roda", Modifier.fillMaxWidth(), selecionado = true) { link.enviarRoda(0x88) }
        Text("Volume dos alto-falantes", color = Apagado, fontSize = 13.sp)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Botao("Volume −", Modifier.weight(1f)) { link.enviarRoda(0x95, 0, 0xFF) }
            Botao("Volume +", Modifier.weight(1f)) { link.enviarRoda(0x95, 0xFF, 0) }
        }
        Text("Farol", color = Apagado, fontSize = 13.sp)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Botao("Liga", Modifier.weight(1f)) { link.enviarRoda(0x73, 0x12) }
            Botao("Desliga", Modifier.weight(1f)) { link.enviarRoda(0x73, 0x13) }
            Botao("Auto", Modifier.weight(1f)) { link.enviarRoda(0x73, 0x14) }
        }
    }
    Secao("Notificação") {
        val prefs = LocalContext.current.getSharedPreferences("s16", android.content.Context.MODE_PRIVATE)
        var qual by remember { mutableStateOf(prefs.getString(ConexaoService.PREF_BUZINA, "roda") ?: "roda") }
        Text("Botão \"Buzina\" da notificação toca:", color = Apagado, fontSize = 13.sp)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            listOf("roda" to "Buzina da roda", "externa" to "Buzina externa").forEach { (chave, nome) ->
                Botao(nome, Modifier.weight(1f), selecionado = qual == chave) {
                    qual = chave
                    prefs.edit().putString(ConexaoService.PREF_BUZINA, chave).apply()
                }
            }
        }
        Text("A notificação mantém a conexão mesmo com o app fechado. Para encerrar, use \"Desconectar\".",
            color = Apagado, fontSize = 13.sp)
    }
}

// ---------------- piscas e lanternas ----------------
@Composable
private fun TelaPiscas(link: C3Link, est: EstadoC3) {
    Secao("Piscas") {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            BotaoGrande("◀  Esquerda", Modifier.weight(1f), est.pisca == 1) { link.enviar(Cmd.PISCA, 1) }
            BotaoGrande("Direita  ▶", Modifier.weight(1f), est.pisca == 2) { link.enviar(Cmd.PISCA, 2) }
        }
        Text("Toque de novo no mesmo lado para desligar.", color = Apagado, fontSize = 13.sp)
    }
    Secao("Lanternas com a roda parada") {
        LUZES.chunked(2).forEachIndexed { linha, par ->
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                par.forEachIndexed { j, nome ->
                    val i = linha * 2 + j
                    Botao(nome, Modifier.weight(1f), selecionado = est.luzBase == i) { link.enviar(Cmd.LUZBASE, i) }
                }
            }
        }
    }
    Secao("Freio") {
        LinhaSwitch("Freio automático (pela desaceleração)", est.freioAuto) {
            link.enviar(Cmd.FREIOAUTO, if (it) 1 else 0)
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                if (est.freio) "FREANDO" else "Luz de freio apagada",
                Modifier.weight(1f),
                color = if (est.freio) Vermelho else Apagado,
                fontWeight = if (est.freio) FontWeight.Bold else FontWeight.Normal,
            )
            OutlinedButton(onClick = { link.enviar(Cmd.FREIOTESTE, 1) }) { Text("Testar") }
        }
    }
}

// ---------------- fitas de LED ----------------
@Composable
private fun TelaFitas(link: C3Link, est: EstadoC3) {
    Secao("Efeito") {
        MODOS_FITA.chunked(2).forEachIndexed { linha, par ->
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                par.forEachIndexed { j, nome ->
                    val i = linha * 2 + j
                    Botao(nome, Modifier.weight(1f), selecionado = est.ledModo == i) { link.enviar(Cmd.LED_MODO, i) }
                }
                if (par.size == 1) Box(Modifier.weight(1f))
            }
        }
        Text("Automático: respira com a roda parada e vira arco-íris andando.", color = Apagado, fontSize = 13.sp)
    }

    Secao("Cor") {
        PALETA.chunked(4).forEachIndexed { linha, cores ->
            Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                cores.forEachIndexed { j, cor ->
                    val i = linha * 4 + j
                    val sel = est.ledCor == i
                    Box(
                        Modifier.size(44.dp)
                            .clip(CircleShape)
                            .background(cor)
                            .border(if (sel) 3.dp else 1.dp, if (sel) Texto else Apagado, CircleShape)
                            .clickable { link.enviar(Cmd.LED_COR, i) },
                    )
                }
            }
        }
        val nomeCor = if (est.ledCor in NOMES_COR.indices) NOMES_COR[est.ledCor] else "personalizada"
        Text("Cor atual: $nomeCor", color = Apagado, fontSize = 13.sp)
    }

    Secao("Brilho") {
        // o slider mexe localmente e so manda ao soltar (cada envio grava na flash do C3)
        var brilho by remember { mutableFloatStateOf(est.ledBrilho.toFloat()) }
        var arrastando by remember { mutableStateOf(false) }
        LaunchedEffect(est.ledBrilho) { if (!arrastando) brilho = est.ledBrilho.toFloat() }
        Text("${brilho.roundToInt()}% (máximo 60%)", color = Texto)
        Slider(
            value = brilho,
            onValueChange = { arrastando = true; brilho = it },
            valueRange = 0f..60f,
            steps = 11,                                          // de 5 em 5
            onValueChangeFinished = { arrastando = false; link.enviar(Cmd.LED_BRILHO, brilho.roundToInt()) },
        )
    }

    Secao("Junto com as luzes") {
        LinhaSwitch("Piscas nas fitas", est.ledPiscas) {
            link.enviar(Cmd.LED_FLAGS, (if (it) 1 else 0) or (if (est.ledFreio) 2 else 0))
        }
        LinhaSwitch("Freio nas fitas", est.ledFreio) {
            link.enviar(Cmd.LED_FLAGS, (if (est.ledPiscas) 1 else 0) or (if (it) 2 else 0))
        }
    }
}

// ---------------- buzina externa (433) ----------------
@Composable
private fun TelaBuzina(link: C3Link) {
    Secao("Buzina externa") {
        BotaoGrande("BUZINA", Modifier.fillMaxWidth(), selecionado = true) { link.enviar(Cmd.BUZEXT, 0xAA) }
        BUZINA.chunked(2).forEach { par ->
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                par.forEach { (nome, codigo) ->
                    Botao(nome, Modifier.weight(1f)) { link.enviar(Cmd.BUZEXT, codigo) }
                }
            }
        }
        Text("O som e o volume ficam guardados na própria buzina.", color = Apagado, fontSize = 13.sp)
    }
}

// ---------------- pecas ----------------
@Composable
private fun Secao(titulo: String, conteudo: @Composable ColumnScope.() -> Unit) {
    Card(colors = CardDefaults.cardColors(containerColor = Painel), modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Text(
                titulo.uppercase(),
                fontSize = 12.sp, letterSpacing = 1.5.sp,
                color = Apagado, fontWeight = FontWeight.SemiBold,
            )
            conteudo()
        }
    }
}

@Composable
private fun Botao(texto: String, modifier: Modifier = Modifier, selecionado: Boolean = false, onClick: () -> Unit) {
    val pad = PaddingValues(horizontal = 6.dp, vertical = 10.dp)
    if (selecionado) Button(onClick = onClick, modifier = modifier, contentPadding = pad) { Text(texto, maxLines = 1) }
    else OutlinedButton(onClick = onClick, modifier = modifier, contentPadding = pad) { Text(texto, color = Texto, maxLines = 1) }
}

@Composable
private fun BotaoGrande(texto: String, modifier: Modifier = Modifier, selecionado: Boolean = false, onClick: () -> Unit) {
    val cores = if (selecionado) ButtonDefaults.buttonColors()
                else ButtonDefaults.buttonColors(containerColor = Fundo, contentColor = Texto)
    Button(onClick = onClick, modifier = modifier.height(64.dp), colors = cores) {
        Text(texto, fontSize = 18.sp, fontWeight = FontWeight.SemiBold)
    }
}

@Composable
private fun LinhaSwitch(texto: String, valor: Boolean, mudou: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(texto, Modifier.weight(1f), color = Texto)
        Switch(checked = valor, onCheckedChange = mudou)
    }
}

@Composable
private fun Aviso(texto: String, botao: String?, acao: () -> Unit) {
    Secao("Atenção") {
        Text(texto, color = Texto)
        if (botao != null) Button(onClick = acao) { Text(botao) }
    }
}
