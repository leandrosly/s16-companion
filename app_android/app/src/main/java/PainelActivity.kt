package systems.leandro.s16

// Tela principal do app. Cuida das permissoes de Bluetooth, guarda o MAC do C3 escolhido
// e conecta sozinho ao abrir. As telas em si ficam em Telas.kt.

import android.Manifest
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.mutableStateOf
import androidx.core.content.ContextCompat

class PainelActivity : ComponentActivity() {

    private lateinit var link: C3Link
    private val temPermissao = mutableStateOf(false)
    private val btLigado = mutableStateOf(true)
    private val macSalvo = mutableStateOf<String?>(null)

    private val pedido = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { r ->
        temPermissao.value = checaPermissao()                    // (notificacao negada nao impede o app)
        conectarSalvo()
    }

    private fun permissoes(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)

    // o que pedimos ao usuario: Bluetooth (obrigatorio) + notificacoes (Android 13+, para ver a notificacao)
    private fun pedir(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            permissoes() + Manifest.permission.POST_NOTIFICATIONS
        else permissoes()

    private fun checaPermissao() = permissoes().all {
        ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        link = C3Link.obter(this)
        val prefs = getSharedPreferences("s16", Context.MODE_PRIVATE)
        macSalvo.value = prefs.getString("mac_c3", null)
        temPermissao.value = checaPermissao()
        val falta = pedir().any { ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED }
        if (falta) pedido.launch(pedir())

        setContent {
            App(
                link = link,
                macSalvo = macSalvo.value,
                temPermissao = temPermissao.value,
                btLigado = btLigado.value,
                pedirPermissao = { pedido.launch(pedir()) },
                salvarMac = { mac ->
                    macSalvo.value = mac
                    prefs.edit().putString("mac_c3", mac).apply()
                },
            )
        }
    }

    override fun onResume() {
        super.onResume()
        val bm = getSystemService(BluetoothManager::class.java)
        btLigado.value = bm?.adapter?.isEnabled == true
        temPermissao.value = checaPermissao()
        conectarSalvo()
    }

    private fun conectarSalvo() {
        if (!temPermissao.value || !btLigado.value) return
        if (link.conexao.value != Conexao.DESCONECTADO) return
        macSalvo.value?.let { link.conectar(it) }
    }

    // Fechar o app NAO desconecta: o servico (notificacao) segura a conexao.
    // Para desconectar de vez: botao "Desconectar" no app.
}
