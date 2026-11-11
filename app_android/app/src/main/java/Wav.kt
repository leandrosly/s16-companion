package systems.leandro.s16

// Le um WAV PCM 16 bits (mono ou estereo -> mono) para uma Amostra.
fun leWav(b: ByteArray): Amostra {
    fun u16(i: Int) = (b[i].toInt() and 0xFF) or ((b[i + 1].toInt() and 0xFF) shl 8)
    fun u32(i: Int) = u16(i) or (u16(i + 2) shl 16)
    require(b.size > 44 && b[0] == 'R'.code.toByte() && b[8] == 'W'.code.toByte()) { "nao e WAV" }
    var i = 12
    var sr = 44100; var canais = 1; var bits = 16
    while (i + 8 <= b.size) {
        val id = String(b, i, 4, Charsets.US_ASCII)
        val tam = u32(i + 4)
        val ini = i + 8
        if (id == "fmt ") { canais = u16(ini + 2); sr = u32(ini + 4); bits = u16(ini + 14) }
        else if (id == "data") {
            require(bits == 16) { "so 16 bits" }
            val fim = minOf(b.size, ini + tam)
            val n = (fim - ini) / (2 * canais)
            val d = FloatArray(n)
            for (k in 0 until n) {
                var s = 0f
                for (c in 0 until canais) s += u16(ini + (k * canais + c) * 2).toShort() / 32768f
                d[k] = s / canais
            }
            return Amostra(d, sr)
        }
        i = ini + tam + (tam and 1)
    }
    error("WAV sem dados")
}
