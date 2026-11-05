package systems.leandro.s16

// Servico em primeiro plano: mantem a conexao com o C3 mesmo com o app fechado ou a tela
// apagada, e mostra uma notificacao fixa com velocidade, bateria e os botoes
//   [◀ Pisca]  [Buzina]  [Pisca ▶]
// Ele liga quando o usuario conecta (C3Link.conectar) e desliga no "Desconectar".
// A buzina da notificacao pode ser a da roda ou a externa (433): escolhida na pagina Roda.

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
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

        /** Preferencia: qual buzina o botao da notificacao toca ("roda" ou "externa"). */
        const val PREF_BUZINA = "buzina_notif"

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
        criarCanal()
        entrarEmPrimeiroPlano()

        // Se o Android matou o app e recriou o servico, a conexao se perdeu: reconecta
        if (!link.querendoConexao) {
            getSharedPreferences("s16", Context.MODE_PRIVATE).getString("mac_c3", null)?.let { link.reconectar(it) }
        }

        // atualiza a notificacao quando a conexao ou o estado do C3 mudam, e a cada 1 s (velocidade)
        escopo.launch { combine(link.conexao, link.estado) { _, _ -> }.collect { atualizar() } }
        escopo.launch { while (isActive) { delay(1_000); atualizar() } }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        entrarEmPrimeiroPlano()                                  // cada inicio precisa confirmar
        when (intent?.action) {
            ACAO_PISCA_ESQ -> link.enviar(Cmd.PISCA, 1)
            ACAO_PISCA_DIR -> link.enviar(Cmd.PISCA, 2)
            ACAO_BUZINA -> {
                val qual = getSharedPreferences("s16", Context.MODE_PRIVATE).getString(PREF_BUZINA, "roda")
                if (qual == "externa") link.enviar(Cmd.BUZEXT, 0xAA) else link.enviarRoda(0x88)
            }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        escopo.cancel()
        super.onDestroy()
    }

    private fun entrarEmPrimeiroPlano() {
        val n = montar()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(ID_NOTIFICACAO, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } else {
            startForeground(ID_NOTIFICACAO, n)
        }
    }

    private fun criarCanal() {
        val canal = NotificationChannel(CANAL, "Conexão com a roda", NotificationManager.IMPORTANCE_LOW).apply {
            description = "Velocidade, bateria e botões de pisca e buzina"
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

        // (o "%" fica fora do format, como na tela da roda)
        val titulo = when (con) {
            Conexao.CONECTADO -> if (temDados) "%.1f km/h".format(r.velocidade) + " · bateria ${r.bateria}%" else "Conectado ao C3"
            Conexao.CONECTANDO -> "Conectando ao C3..."
            else -> "C3 desconectado: tentando de novo..."
        }
        val texto = when {
            con != Conexao.CONECTADO -> "Ligue a roda"
            est.pisca == 1 -> "◀ Pisca esquerdo ligado"
            est.pisca == 2 -> "Pisca direito ligado ▶"
            est.freio -> "Freando"
            else -> "Toque para abrir o S16 Companion"
        }
        val abrir = PendingIntent.getActivity(
            this, 0,
            Intent(this, PainelActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        return NotificationCompat.Builder(this, CANAL)
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
            .addAction(0, if (est.pisca == 1) "◀ PISCA" else "◀ Pisca", acao(ACAO_PISCA_ESQ, 1))
            .addAction(0, "Buzina", acao(ACAO_BUZINA, 2))
            .addAction(0, if (est.pisca == 2) "PISCA ▶" else "Pisca ▶", acao(ACAO_PISCA_DIR, 3))
            .build()
    }
}
