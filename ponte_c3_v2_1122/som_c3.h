// =====================================================================================
//  SOM DO C3 - motores e buzinas tocando no PCM5102A (I2S) -> PAM8302 -> alto-falante
// =====================================================================================
// Versao enxuta do motor de som do app (SomMotor.kt) para o ESP32-C3:
//  * o C3 nao tem unidade de ponto flutuante: tudo que roda por amostra e em numeros inteiros
//    (ponto fixo); so o "controle" (a cada ~12 ms) usa float.
//  * as gravacoes (Harley estudio, buzinas, pneu) ficam na flash em mu-law de 8 bits, 22050 Hz
//    (sons_c3.h, ~1,1 MB).
//  * roda numa tarefa propria do FreeRTOS: o loop(), o BLE e o Serial nao conseguem atrasar o som.
//
// Motores: 0 = Eletrico VESS (sintetizado, como no app)  1 = Harley estudio (gravacoes)
// Buzinas (indice): 0 Ahooga, 1 Carro antigo, 2 Mustang, 3 Mega, 4 Trem, 5 Fon-fon, 6 Bicicleta, 7 Fon-fon 2 tons
// Pneu cantando nas freadas fortes (gravacoes, vale para os dois motores).
//
// Uso no .ino:
//   som::begin(PINO_BCK, PINO_DIN, PINO_LCK, PINO_AMP);   // no setup()
//   som::roda(velocidade, corrente, quandoMs);             // a cada dado novo da roda (ou no loop)
//   som::ligar(true) | som::motor(1) | som::volume(70) | som::buzina(7, true/false) | som::pneu(true)
//   som::simular(25)   // testa sem a roda (velocidade fixa em km/h); som::simular(NAN) volta para a roda
//
// O mesmo arquivo compila no PC (sem ARDUINO) para testar o som gerando um WAV.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "sons_c3.h"

namespace som {

static const int SR = 22050;            // taxa de saida (as gravacoes ja estao nela)
static const int BLOCO = 64;            // amostras por bloco (~2,9 ms)
static const int CONTROLE = 4;          // blocos por passo de controle (~11,6 ms)

// ---------------- tabelas ----------------
static int16_t MULAW[256];
static int16_t SENO[1024];
static void tabelas() {
  for (int q = 0; q < 256; q++) {
    float y = q / 255.0f * 2.0f - 1.0f;
    float x = (y < 0 ? -1.0f : 1.0f) * (powf(256.0f, fabsf(y)) - 1.0f) / 255.0f;
    MULAW[q] = (int16_t)lrintf(x * 32767.0f);
  }
  for (int i = 0; i < 1024; i++) SENO[i] = (int16_t)lrintf(sinf(6.2831853f * i / 1024.0f) * 32767.0f);
}

static const Gravacao* grav(const char* nome) {
  for (int i = 0; i < N_GRAVACOES; i++) if (strcmp(GRAVACOES[i].nome, nome) == 0) return &GRAVACOES[i];
  return nullptr;
}

static inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
static inline float dbLin(float db) { return db <= -90.0f ? 0.0f : powf(10.0f, db / 20.0f); }

// valor que vai ate o alvo exponencialmente (setTargetAtTime); avanca por bloco
struct Suave {
  float v = 0, alvo = 0, k = 1;          // k = fracao que anda por bloco
  void para(float a, float tau) { alvo = a; k = tau <= 0 ? 1.0f : 1.0f - expf(-(float)BLOCO / (tau * SR)); }
  void ja(float a) { v = alvo = a; }
  float avanca() { v += (alvo - v) * k; return v; }
};

static uint32_t semente = 22222;
static inline int32_t ruido() {             // branco, -32768..32767
  semente ^= semente << 13; semente ^= semente >> 17; semente ^= semente << 5;
  return (int32_t)(semente >> 16) - 32768;
}
static inline float rnd() { ruido(); return (semente >> 8) / 16777216.0f; }

// ---------------- toca uma gravacao (posicao em ponto fixo 20.12) ----------------
struct Voz {
  const Gravacao* g = nullptr;
  uint32_t pos = 0, passo = 4096;       // Q12 (4096 = velocidade normal)
  bool laco = false; uint32_t ls = 0, le = 0;   // laco em amostras
  uint32_t fim = 0;                     // sem laco: para aqui (amostras)
  int32_t atraso = 0;                   // amostras antes de comecar
  int32_t entrada = 0, nIn = 0;         // rampa de entrada
  int32_t saidaEm = -1, saidaDur = 1, nOut = 0;
  Suave ganho, taxa;
  bool ativa = false;

  void inicia(const Gravacao* gr) {
    g = gr; pos = 0; passo = 4096; laco = false; ls = 0; le = gr ? gr->n : 0; fim = le;
    atraso = 0; entrada = 0; nIn = 0; saidaEm = -1; saidaDur = 1; nOut = 0;
    ganho.ja(1); taxa.ja(1); ativa = gr != nullptr;
  }
  void comecaEm(float seg) { pos = (uint32_t)(seg * SR) << 12; }
  void ateSeg(float seg) { uint32_t f = (uint32_t)(seg * SR); if (f < fim) fim = f; }
  void lacoEm(float a, float b) { laco = true; ls = (uint32_t)(a * SR); le = (uint32_t)(b * SR); }
  void sai(int32_t em, int32_t dur) { if (saidaEm >= 0) return; saidaEm = em < 0 ? 0 : em; saidaDur = dur < 1 ? 1 : dur; }

  // soma n amostras em acc (escala 1.0 = 32767)
  void soma(int32_t* acc, int n) {
    if (!ativa) return;
    float g0 = ganho.v, g1 = ganho.avanca();
    float t1 = taxa.avanca();
    passo = (uint32_t)(t1 * 4096.0f);
    int32_t gq = (int32_t)(g0 * 4096.0f), dg = (int32_t)((g1 - g0) * 4096.0f) / n;
    const uint8_t* d = g->d; const uint32_t ult = g->n - 1;
    for (int i = 0; i < n; i++) {
      gq += dg;
      if (atraso > 0) { atraso--; continue; }
      int32_t env = 4096;
      if (entrada > 0 && nIn < entrada) { env = (int32_t)(((int64_t)nIn * 4096) / entrada); nIn++; }
      if (saidaEm >= 0) {
        if (saidaEm > 0) saidaEm--;
        else { nOut++; if (nOut >= saidaDur) { ativa = false; return; }
               env = (int32_t)(((int64_t)env * (saidaDur - nOut)) / saidaDur); }
      }
      uint32_t ip = pos >> 12, fr = pos & 4095;
      if (ip >= ult) { if (!laco) { ativa = false; return; } ip = ult; }
      int32_t a = MULAW[d[ip]], b = MULAW[d[ip < ult ? ip + 1 : ip]];
      int32_t s = a + (((b - a) * (int32_t)fr) >> 12);
      acc[i] += (((s * gq) >> 12) * env) >> 12;
      pos += passo;
      if (laco) { if ((pos >> 12) >= le) pos -= (le - ls) << 12; }
      else if ((pos >> 12) >= fim) { ativa = false; return; }
    }
  }
};

// ---------------- filtros de 1 polo em ponto fixo ----------------
struct PassaBaixa {                     // y += (x - y) * k
  int32_t y = 0, k = 32767;
  void freq(float f) { k = (int32_t)((1.0f - expf(-6.2831853f * f / SR)) * 32767.0f); }
  inline int32_t proc(int32_t x) { y += (int32_t)(((int64_t)(x - y) * k) >> 15); return y; }
};
struct PassaAlta {                      // x - passa-baixa(x)
  PassaBaixa pb;
  void freq(float f) { pb.freq(f); }
  inline int32_t proc(int32_t x) { return x - pb.proc(x); }
};

// ---------------- estado que os motores recebem ----------------
struct Estado { float v = 0, a = 0, load = 0, rpm = 0, rpmN = 0; bool estouros = true; };

struct Motor {
  Suave out;
  bool parando = false; int32_t parado = 0;      // amostras desde o parar()
  bool acabou = false;
  int32_t buf[BLOCO];
  virtual ~Motor() {}
  virtual void update(const Estado& s) = 0;
  virtual void gera(int n) = 0;                  // escreve em buf (sobrescreve)
  virtual void parar(bool comSom) { parando = true; out.para(0, 0.06f); }
  virtual bool extrasTocando() { return false; }
  virtual void extras(int32_t* acc, int n) {}
  void mistura(int32_t* acc, int n) {
    if (!(parando && parado > SR * 45 / 100)) {
      gera(n);
      float g0 = out.v, g1 = out.avanca();
      int32_t gq = (int32_t)(g0 * 4096), dg = (int32_t)((g1 - g0) * 4096) / n;
      for (int i = 0; i < n; i++) { gq += dg; acc[i] += (int32_t)(((int64_t)buf[i] * gq) >> 12); }
    }
    if (parando) parado += n;
    extras(acc, n);
    if (parando && parado > SR * 45 / 100 && !extrasTocando()) acabou = true;
  }
};

// ======================= Eletrico VESS (modelo medido no Hyundai Kona) =======================
static const float VESS_V[8] = {0, 1.8f, 6.9f, 12.3f, 21, 24.3f, 29.8f, 45};
struct TomVess { float f0; float tab[8]; bool marcha; };
static const TomVess VESS_TONS[17] = {
  {98.0f,   {-99,-99,-14,-5,6,8,2,-6}, false},     {150.7f,  {-99,-7,4,13,15,15,-12,-30}, true},
  {172.3f,  {-23,-17,-20,-14,-7,-3,-9,-15}, false}, {199.0f,  {-99,-11,-2,2,6,7,-5,-12}, false},
  {218.0f,  {-14,-18,-9,-8,-4,-9,-5,-12}, false},   {261.1f,  {-16,-13,-12,-9,-4,-3,-9,-15}, false},
  {349.9f,  {-3,-1,-6,5,5,6,-11,-18}, false},       {395.7f,  {-27,-28,-22,-15,-8,-5,-11,-18}, false},
  {436.0f,  {-9,-13,-10,-11,-1,-6,-9,-15}, false},  {527.6f,  {0,2,2,3,3,10,-6,-12}, false},
  {654.1f,  {-7,-6,-4,-6,8,1,-9,-15}, false},       {697.1f,  {-11,-17,-9,3,2,-2,-13,-20}, false},
  {786.0f,  {-5,-4,-5,-11,-6,-5,-12,-18}, false},   {877.5f,  {-17,-15,-6,-22,-10,-10,-17,-24}, false},
  {1044.4f, {-25,-20,-10,-6,1,-1,-17,-24}, false},  {1335.0f, {-99,-21,-17,-6,-2,2,-10,-18}, false},
  {1566.5f, {-99,-14,-14,-6,-9,-6,-27,-30}, false},
};
static const float VESS_MARROM[8] = {-30,-18,-10,1,5,6,13,15};
static const float VESS_ROSA[8]   = {-43,-37,-28,-27,-25,-23,-13,-10};
static const float VESS_CHIADO[8] = {-55,-46,-36,-30,-27,-25,-21,-19};
static const float VESS_APITO[8]  = {-99,-36,-31,-34,-46,-99,-99,-99};
static float vessInterp(const float* tab, float v) {
  float x = v > VESS_V[7] ? VESS_V[7] : v;
  for (int i = 1; i < 8; i++) if (x <= VESS_V[i]) {
    float r = (x - VESS_V[i - 1]) / (VESS_V[i] - VESS_V[i - 1]);
    return tab[i - 1] + (tab[i] - tab[i - 1]) * r;
  }
  return tab[7];
}

struct MotorVess : Motor {
  // 3 vozes por tom (no app: centro + duas desafinadas nos lados; aqui tudo em mono)
  static const int NV = 3;
  uint32_t fase[17][NV] = {}, inc[17][NV] = {};
  float off[17][NV];
  Suave g[17];
  int32_t amp[NV] = { 2896, 1323, 843 };          // Q12: 0,707 / 0,323 / 0,206 (soma mono do pan do app)
  // ruidos (mono): marrom (passa-baixa 1200), rosa (120..1800), chiado (acima de 8,3 kHz)
  int32_t marromY = 0; PassaBaixa marromPb; Suave gMarrom;
  int32_t r0 = 0, r1 = 0, r2 = 0; PassaAlta rosaPa1, rosaPa2; PassaBaixa rosaPb1, rosaPb2; Suave gRosa;
  int32_t chX1 = 0, chX2 = 0; Suave gChiado;      // chiado: diferenca dupla do branco (sobe 12 dB/oitava)
  uint32_t faseAp[3] = {}, incAp[3] = {}; Suave gApito;
  PassaAlta brilhoPa; Suave brilho;
  uint32_t faseLfo = 0, incLfo = hz(0.45f);
  // calibracao dos ruidos (x16 = 1): iguala o nivel de cada um ao do app (medido no PC), ja somado em mono
  int32_t KM = 104, KR = 109, KC = 21;

  MotorVess() {
    out.ja(0); out.para(0.03f, 0.08f);
    for (int t = 0; t < 17; t++) {
      off[t][0] = 0; off[t][1] = 1.5f + rnd() * 3; off[t][2] = -(1.5f + rnd() * 3);
      for (int k = 0; k < NV; k++) inc[t][k] = hz(VESS_TONS[t].f0 + off[t][k]);
      g[t].ja(0);
    }
    marromPb.freq(1200); rosaPa1.freq(120); rosaPa2.freq(120); rosaPb1.freq(1800); rosaPb2.freq(1800);
    brilhoPa.freq(900);
  }
  static uint32_t hz(float f) { return (uint32_t)(f / SR * 4294967296.0f); }

  void update(const Estado& s) override {
    float v = s.v < 0 ? 0 : s.v;
    float vv = v > 45 ? 45 : v;
    float k = 1 + 0.33f * powf(vv / 30.0f, 1.5f);
    float vI = v <= 8 ? v : 8 + (v - 8) * (19.0f / 27.0f);
    float fade = clampf((v - 2) / 4, 0, 1); fade *= fade;
    float marcha = v > 17.6f ? 1.0f / 1.125f : 1.0f;
    for (int t = 0; t < 17; t++) {
      float f = VESS_TONS[t].f0 * k;
      for (int j = 0; j < NV; j++) {
        float fj = f + off[t][j];
        if (VESS_TONS[t].marcha) fj *= marcha;
        // (o app suaviza a frequencia em 0,05-0,12 s; aqui o passo de controle ja e suave o bastante)
        inc[t][j] = hz(fj);
      }
      g[t].para(dbLin(vessInterp(VESS_TONS[t].tab, vI)), 0.08f);
    }
    gMarrom.para(dbLin(vessInterp(VESS_MARROM, vI)) * 1.2f, 0.1f);
    gRosa.para(fade * dbLin(vessInterp(VESS_ROSA, vI)) * 3.0f, 0.1f);
    gChiado.para(fade * dbLin(vessInterp(VESS_CHIADO, vI)) * 3.4f, 0.1f);
    gApito.para(dbLin(vessInterp(VESS_APITO, v)), 0.1f);
    float fa = 1300 + 260 * v; if (fa > 10000) fa = 10000;
    incAp[0] = hz(fa); incAp[1] = hz(fa * 1.12f); incAp[2] = hz(fa * 1.26f);
    brilho.para(powf(10.0f, 6.0f * s.load / 20.0f) - 1.0f, 0.08f);
  }

  void gera(int n) override {
    memset(buf, 0, sizeof(int32_t) * n);
    // tons
    for (int t = 0; t < 17; t++) {
      float g0 = g[t].v, g1 = g[t].avanca();
      if (g0 < 1e-5f && g1 < 1e-5f) continue;
      int32_t gq = (int32_t)(g0 * 4096), dg = (int32_t)((g1 - g0) * 4096) / n;
      for (int i = 0; i < n; i++) {
        gq += dg;
        int32_t s = 0;
        for (int j = 0; j < NV; j++) { fase[t][j] += inc[t][j]; s += SENO[fase[t][j] >> 22] * amp[j]; }
        buf[i] += (int32_t)(((int64_t)(s >> 12) * gq) >> 12);
      }
    }
    // ruidos (normalizados para ~rms 1 = 32767*0,3 antes do ganho)
    float m0 = gMarrom.v, m1 = gMarrom.avanca(), p0 = gRosa.v, p1 = gRosa.avanca(), c0 = gChiado.v, c1 = gChiado.avanca();
    float a0 = gApito.v, a1 = gApito.avanca(), b0 = brilho.v, b1 = brilho.avanca();
    int32_t mq = (int32_t)(m0 * 4096), dm = (int32_t)((m1 - m0) * 4096) / n;
    int32_t pq = (int32_t)(p0 * 4096), dp = (int32_t)((p1 - p0) * 4096) / n;
    int32_t cq = (int32_t)(c0 * 4096), dc = (int32_t)((c1 - c0) * 4096) / n;
    int32_t aq = (int32_t)(a0 * 4096), da = (int32_t)((a1 - a0) * 4096) / n;
    int32_t bq = (int32_t)(b0 * 4096), db = (int32_t)((b1 - b0) * 4096) / n;
    for (int i = 0; i < n; i++) {
      mq += dm; pq += dp; cq += dc; aq += da; bq += db;
      int32_t w = ruido();
      // marrom: integrador com perda, depois passa-baixa 1200 Hz
      marromY += (w >> 4) - (marromY >> 7);
      int32_t mr = marromPb.proc(marromY >> 1);
      // rosa: 3 polos (aproximacao de Kellet) + faixa 120..1800 Hz
      r0 = r0 - (r0 / 128) + (w / 32); r1 = r1 - (r1 / 16) + (w / 16); r2 = r2 - (r2 / 2) + (w / 4);
      int32_t ro = rosaPb2.proc(rosaPb1.proc(rosaPa2.proc(rosaPa1.proc((r0 + r1 + r2) >> 1))));
      // chiado: diferenca dupla do branco (concentra a energia perto dos 8-11 kHz)
      int32_t wn = ruido() >> 1, ch = wn - 2 * chX1 + chX2; chX2 = chX1; chX1 = wn;
      int32_t y = buf[i] + (int32_t)(((int64_t)mr * mq * KM) >> 16) + (int32_t)(((int64_t)ro * pq * KR) >> 16)
                         + (int32_t)(((int64_t)ch * cq * KC) >> 16);
      if (aq > 0) {                    // 3 apitos (mono: 0,653 / 0,457 / 0,354)
        faseAp[0] += incAp[0]; faseAp[1] += incAp[1]; faseAp[2] += incAp[2];
        int32_t ap = (SENO[faseAp[0] >> 22] * 2675 + SENO[faseAp[1] >> 22] * 1872 + SENO[faseAp[2] >> 22] * 1450) >> 12;
        y += (ap * aq) >> 12;
      }
      // brilho (agudo +6 dB * carga) e a oscilacao lenta de +-10%
      y += (int32_t)(((int64_t)brilhoPa.proc(y) * bq) >> 12);
      faseLfo += incLfo;
      y += (int32_t)(((int64_t)y * (SENO[faseLfo >> 22] / 10)) >> 15);
      buf[i] = y;
    }
  }
};

// ======================= Harley estudio (banco de gravacoes) =======================
struct MotorBanco : Motor {
  struct Laco { Voz voz; float rpm; };
  Laco lacos[4]; int nLacos = 0;
  Voz partida, desliga;
  static const int MAX_EST = 6; Voz est[MAX_EST];
  PassaBaixa lp; Suave fLp;
  int32_t tmp[BLOCO];
  float cargaAnt = 0; int rajada = 0; float idle = 1090;
  const Gravacao* gEst;

  MotorBanco(bool ligando) {
    out.ja(0.85f); out.para(0.85f, 0.08f);
    const char* nomes[4] = {"h_e_lenta", "h_e_1600", "h_e_1900", "h_e_4000"};
    const float rpms[4] = {1090, 1620, 1900, 4000};
    float inicio = 0.02f;
    if (ligando) {
      const Gravacao* p = grav("h_e_partida");
      if (p) { partida.inicia(p); partida.atraso = (int32_t)(0.02f * SR); inicio = 0.02f + (float)p->n / SR - 0.01f; }
    }
    for (int i = 0; i < 4; i++) {
      const Gravacao* g = grav(nomes[i]); if (!g) continue;
      Laco& l = lacos[nLacos++];
      l.voz.inicia(g); l.voz.laco = true; l.voz.atraso = (int32_t)(inicio * SR); l.voz.entrada = (int32_t)(0.02f * SR);
      l.voz.ganho.ja(i == 0 ? 1 : 0); l.rpm = rpms[i];
    }
    gEst = grav("h_estouros");
    fLp.ja(6000); lp.freq(6000);
  }

  void update(const Estado& s) override {
    if (!nLacos) return;
    float R = s.rpm < 300 ? 300 : s.rpm; int n = nLacos, k = 0;
    while (k < n - 2 && R > lacos[k + 1].rpm) k++;
    for (int i = 0; i < n; i++) {
      float w = 0;
      if (n == 1 || R <= lacos[0].rpm) w = i == 0 ? 1 : 0;
      else if (R >= lacos[n - 1].rpm) w = i == n - 1 ? 1 : 0;
      else {
        float x = (logf(R) - logf(lacos[k].rpm)) / (logf(lacos[k + 1].rpm) - logf(lacos[k].rpm));
        if (i == k) w = cosf(x * 1.5707963f); else if (i == k + 1) w = sinf(x * 1.5707963f);
      }
      lacos[i].voz.ganho.para(w, 0.05f);
      lacos[i].voz.taxa.para(clampf(R / lacos[i].rpm, 0.6f, 2.6f), 0.03f);
    }
    fLp.para(5000 + (0.4f * s.rpmN + 0.6f * s.load) * 9000, 0.05f);
    out.para(0.85f + 0.3f * s.load, 0.05f);
    if (s.estouros) {                    // acelerador fechado com a rotacao acima da lenta
      float dt = (float)BLOCO * CONTROLE / SR;
      if (cargaAnt > 0.45f && s.load < 0.12f) rajada = 2 + (int)(rnd() * 3);
      cargaAnt = s.load;
      float acima = clampf((s.rpm - idle * 1.15f) / 1000, 0, 1);
      bool desacel = s.load < 0.1f && s.a < -0.8f && acima > 0;
      if (desacel || rajada > 0) {
        float m = -s.a * 0.8f; if (m > 8) m = 8;
        float tx = (2 + m) * (acima > (rajada > 0 ? 0.6f : 0) ? acima : (rajada > 0 ? 0.6f : 0));
        if (rnd() < tx * dt) { estouro(0.45f + rnd() * 0.6f); if (rajada > 0) rajada--; }
      }
    }
  }
  void estouro(float amp) {
    if (!gEst) return;
    for (int i = 0; i < MAX_EST; i++) if (!est[i].ativa) {
      int q = (int)(rnd() * 20); if (q > 19) q = 19;
      est[i].inicia(gEst); est[i].comecaEm(q * 0.103f); est[i].ateSeg(q * 0.103f + 0.098f);
      est[i].taxa.ja(0.85f + rnd() * 0.3f); est[i].ganho.ja(amp); est[i].atraso = (int32_t)(rnd() * 0.015f * SR);
      return;
    }
  }
  void parar(bool comSom) override {
    Motor::parar(comSom);
    out.para(0, comSom ? 0.03f : 0.06f);
    if (comSom) { const Gravacao* g = grav("h_e_desliga"); if (g) desliga.inicia(g); }
  }
  bool extrasTocando() override { return desliga.ativa; }
  void extras(int32_t* acc, int n) override { desliga.soma(acc, n); }
  void gera(int n) override {
    memset(tmp, 0, sizeof(int32_t) * n);
    for (int i = 0; i < nLacos; i++) lacos[i].voz.soma(tmp, n);
    lp.freq(fLp.avanca());
    for (int i = 0; i < n; i++) buf[i] = lp.proc(tmp[i]);
    partida.soma(buf, n);
    for (int i = 0; i < MAX_EST; i++) est[i].soma(buf, n);
  }
};

// ======================= buzinas e pneu =======================
struct Buzina { const char* grav; const char* nome; float ls, le; float soltaTaxa; };   // ls<0 = sem laco
static const Buzina BUZINAS[8] = {
  {"b_ahooga",  "Ahooga",        0.48104f, 1.11233f, 0},
  {"b_vintage", "Carro antigo",  0.21792f, 0.90462f, 0},
  {"b_mustang", "Mustang 1985",  0.17371f, 0.48267f, 0},
  {"b_mega",    "Mega buzina",   0.72958f, 2.20471f, 0},
  {"b_trem",    "Trem",          1.43725f, 3.742f,   0},
  {"b_bulb",    "Fon-fon",       -1, -1, 0},
  {"b_bici",    "Bicicleta",     -1, -1, 0},
  {"b_bulb",    "Fon-fon 2 tons", -1, -1, 1.19f},
};

// ======================= a mesa: simulacao + motor + buzinas + pneu + saida =======================
struct Mesa {
  // ajustes
  float volume = 0.7f; bool estouros = true; bool pneuOn = true; float reacao = 1;
  // simulacao
  float v = 0, vAnt = 0, a = 0, load = 0, rpm = 0, corte = 0; int marcha = 0;
  float simAlvo = NAN;                  // NAN = segue a roda
  // amostras da roda
  uint32_t rodaQuando = 0; float rodaV = 0, rodaInc = 0, rodaIntervalo = 0.2f, desde = 0;
  // motor
  int motorId = 0; bool ligado = false;
  Motor* motor = nullptr; Motor* saindo = nullptr;
  // buzinas
  static const int NB = 4; Voz buz[NB]; Voz* segura = nullptr; int seguraIdx = -1; int32_t seguraT = 0;
  // pneu
  Voz pneu[2]; Suave gPneu; float tPneu = 0;
  // saida
  Suave vol;
  int fase = 0;
  int32_t acc[BLOCO];

  void init() {
    tabelas();
    const char* p[2] = {"p_pneu1", "p_pneu2"};
    for (int i = 0; i < 2; i++) {
      const Gravacao* g = grav(p[i]); if (!g) continue;
      pneu[i].inicia(g); pneu[i].laco = true; pneu[i].pos = (uint32_t)(g->n * 0.37f * i) << 12;
      pneu[i].ganho.ja(i == 0 ? 0.6f : 0.45f);
    }
    vol.ja(volume);
  }

  bool tocando() {
    if (ligado || saindo) return true;
    for (int i = 0; i < NB; i++) if (buz[i].ativa) return true;
    return gPneu.v > 0.001f;
  }

  Motor* cria(int id, bool ligando) { return id == 1 ? (Motor*)new MotorBanco(ligando) : (Motor*)new MotorVess(); }
  bool rpmMotor() { return motorId == 1; }

  void ligar(bool on) {
    if (on == ligado) return;
    ligado = on;
    if (on) { rpm = rpmMotor() ? 1090 : 0; marcha = 0; motor = cria(motorId, true); }
    else if (motor) { motor->parar(true); if (saindo) delete saindo; saindo = motor; motor = nullptr; }
  }
  void escolher(int id) {
    if (id < 0 || id > 1 || id == motorId) return;
    motorId = id; rpm = rpmMotor() ? 1090 : 0; marcha = 0;
    if (ligado) {
      if (motor) { motor->parar(false); if (saindo) delete saindo; saindo = motor; }
      motor = cria(id, false);
    }
  }

  void apertaBuzina(int idx) {
    if (idx < 0 || idx > 7) return;
    const Buzina& b = BUZINAS[idx];
    const Gravacao* g = grav(b.grav); if (!g) return;
    soltaBuzina();
    Voz* v = nullptr;
    for (int i = 0; i < NB; i++) if (!buz[i].ativa) { v = &buz[i]; break; }
    if (!v) v = &buz[0];
    v->inicia(g);
    if (b.ls >= 0) v->lacoEm(b.ls, b.le);
    if (b.ls >= 0 || b.soltaTaxa > 0) { segura = v; seguraIdx = idx; seguraT = 0; }
  }
  void soltaBuzina() {
    if (!segura) return;
    const Buzina& b = BUZINAS[seguraIdx];
    Voz* v = segura; segura = nullptr;
    const Gravacao* g = v->g;
    Voz* r = nullptr;
    for (int i = 0; i < NB; i++) if (!buz[i].ativa) { r = &buz[i]; break; }
    int32_t x = (int32_t)(0.025f * SR);
    if (b.soltaTaxa > 0) {                         // o segundo "fon", em outro tom
      int32_t esp = (int32_t)(0.13f * SR) - seguraT; if (esp < 0) esp = 0;
      if (r) { r->inicia(g); r->taxa.ja(b.soltaTaxa); r->atraso = esp; }
      v->sai(esp, (int32_t)(0.02f * SR));
      return;
    }
    int32_t esp = (int32_t)(b.ls * SR) - seguraT; if (esp < 0) esp = 0;   // pelo menos o ataque inteiro
    if (r) { r->inicia(g); r->comecaEm(b.le); r->atraso = esp; r->entrada = x; }
    v->sai(esp, x);
  }

  // dados da roda (ou NAN) a cada passo de controle
  void passo(float dt, float vRoda, float corrente, uint32_t quando) {
    if (!isnan(simAlvo)) {
      float diff = simAlvo - v, tx = diff > 0 ? 7.0f : 18.0f;
      float m = fabsf(diff) / 4 + 0.15f; if (m > 1) m = 1;
      float d = tx * dt * m; if (d > fabsf(diff)) d = fabsf(diff);
      v += diff > 0 ? d : -d;
      rodaQuando = 0;
    } else if (!isnan(vRoda)) {
      float vr = fabsf(vRoda);
      if (quando != rodaQuando) {
        if (rodaQuando != 0) {
          float iv = clampf((quando - rodaQuando) / 1000.0f, 0.03f, 1.0f);
          rodaIntervalo += (iv - rodaIntervalo) * 0.3f;
          rodaInc += (clampf((vr - rodaV) / iv, -40, 40) - rodaInc) * 0.6f;
        }
        rodaQuando = quando; rodaV = vr; desde = 0;
      }
      desde += dt;
      float prev = rodaV + rodaInc * (desde < rodaIntervalo * 1.2f ? desde : rodaIntervalo * 1.2f);
      if (prev < 0) prev = 0;
      float k = dt / 0.04f; if (k > 1) k = 1;
      v += (prev - v) * k;
    } else {
      v += (0 - v) * (dt * 2 > 1 ? 1 : dt * 2);          // sem dados: desacelera ate parar
    }
    if (dt > 0) { float k = dt * 6; if (k > 1) k = 1; a += ((v - vAnt) / dt - a) * k; }
    vAnt = v;
    float la = clampf(a / (6.0f / reacao), 0, 1);
    if (v > 2 && a > -0.5f && la < 0.18f) la = 0.18f;
    if (!isnan(corrente) && corrente > 0) { float c = clampf(corrente / 30, 0, 1); if (c > la) la = c; }
    if (corte > 0) { la = 0; corte -= dt; }
    { float k = dt * 8; if (k > 1) k = 1; load += (la - load) * k; }
    // rotacao (Harley estudio: 4 marchas)
    if (rpmMotor()) {
      static const float G[4] = {150, 80, 46, 35};
      const float idle = 1090, mx = 5500, up = 3600, upLeve = 2500, down = 1500, loadRpm = 500;
      float r = idle + v * G[marcha];
      float u = load < 0.3f ? upLeve : up;
      if (r > u && marcha < 3) { marcha++; corte = 0.14f; }
      else if (r < down && marcha > 0) marcha--;
      float alvo = idle + v * G[marcha] + load * loadRpm * (v < 8 ? 1.6f : 1.0f);
      if (alvo > mx) alvo = mx;
      float k = dt * (alvo > rpm ? 6 : 4); if (k > 1) k = 1;
      rpm += (alvo - rpm) * k;
    } else {
      float alvo = (v / 50) * 75 + load * 25; if (alvo > 100) alvo = 100;
      float k = dt * 5; if (k > 1) k = 1;
      rpm += (alvo - rpm) * k;
    }
    // pneu: freadas fortes (acima de 9 km/h por segundo, cheio em 19), so andando
    float forca = clampf((-a - 9) / 10, 0, 1) * clampf((v - 3) / 5, 0, 1);
    float alvoP = pneuOn ? forca : 0;
    gPneu.para(alvoP, alvoP > gPneu.v ? 0.05f : 0.12f);
    tPneu += dt;
    for (int i = 0; i < 2; i++) pneu[i].taxa.para(0.95f + 0.1f * forca + 0.03f * sinf(tPneu * (1.3f + i * 0.7f)), 0.1f);
  }

  // gera n amostras (int16 mono). vRoda/corrente: NAN se nao ha dados da roda.
  void gera(int16_t* out, int n, float vRoda, float corrente, uint32_t quando) {
    int feito = 0;
    while (feito < n) {
      if (fase == 0) {
        float dt = (float)BLOCO * CONTROLE / SR;
        passo(dt, vRoda, corrente, quando);
        Estado s; s.v = v; s.a = a; s.load = load; s.rpm = rpm; s.rpmN = rpm / (rpmMotor() ? 5500.0f : 100.0f); s.estouros = estouros;
        if (motor) motor->update(s);
        if (saindo) saindo->update(s);
        vol.para(volume, 0.03f);
      }
      fase = (fase + 1) % CONTROLE;
      int k = n - feito < BLOCO ? n - feito : BLOCO;
      memset(acc, 0, sizeof(int32_t) * k);
      if (motor) motor->mistura(acc, k);
      if (saindo) { saindo->mistura(acc, k); if (saindo->acabou) { delete saindo; saindo = nullptr; } }
      for (int i = 0; i < NB; i++) buz[i].soma(acc, k);
      seguraT += k;
      // pneu
      float p0 = gPneu.v, p1 = gPneu.avanca();
      if (p0 > 1e-4f || p1 > 1e-4f) {
        static int32_t tp[BLOCO];
        memset(tp, 0, sizeof(int32_t) * k);
        pneu[0].soma(tp, k); pneu[1].soma(tp, k);
        int32_t gq = (int32_t)(p0 * 4096), dg = (int32_t)((p1 - p0) * 4096) / k;
        for (int i = 0; i < k; i++) { gq += dg; acc[i] += (tp[i] * gq) >> 12; }
      }
      // volume + limitador suave
      float g0 = vol.v, g1 = vol.avanca();
      int32_t gq = (int32_t)(g0 * 4096), dg = (int32_t)((g1 - g0) * 4096) / k;
      for (int i = 0; i < k; i++) {
        gq += dg;
        int32_t y = (int32_t)(((int64_t)acc[i] * gq) >> 12);
        int32_t ay = y < 0 ? -y : y;
        if (ay > 22000) { ay = 22000 + (ay - 22000) / 3; if (ay > 32000) ay = 32000; y = y < 0 ? -ay : ay; }
        out[feito + i] = (int16_t)y;
      }
      feito += k;
    }
  }
};

#ifdef ARDUINO
// =====================================================================================
//  Parte do ESP32: I2S + tarefa do FreeRTOS + comandos
// =====================================================================================
}  // namespace som
#include <Arduino.h>
#include "driver/i2s_std.h"
namespace som {

struct Cmd { uint8_t tipo; int16_t a; float f; };
enum { CMD_LIGAR = 1, CMD_MOTOR, CMD_VOLUME, CMD_BUZ_APERTA, CMD_BUZ_SOLTA, CMD_PNEU, CMD_SIMULAR, CMD_ESTOUROS };
static QueueHandle_t fila = nullptr;
static Mesa mesa;
static i2s_chan_handle_t tx = nullptr;
static int pinoAmp = -1;
static volatile float rodaV = NAN, rodaI = NAN;
static volatile uint32_t rodaT = 0;
// estado para quem pergunta (escrito so pela tarefa)
static volatile bool estLigado = false, estTocando = false;
static volatile uint8_t estMotor = 0, estVol = 70, estPneu = 1;
static volatile float estV = 0, estRpm = 0;
static volatile int estMarcha = 0;
static volatile uint32_t estCarga = 0;     // % do tempo de CPU da tarefa (gerando som)

static void manda(uint8_t t, int16_t a = 0, float f = 0) { if (fila) { Cmd c{t, a, f}; xQueueSend(fila, &c, 0); } }
static void ligar(bool on) { manda(CMD_LIGAR, on); }
static void motor(int id) { manda(CMD_MOTOR, id); }
static void volume(int pct) { manda(CMD_VOLUME, pct); }
static void buzina(int idx, bool aperta) { manda(aperta ? CMD_BUZ_APERTA : CMD_BUZ_SOLTA, idx); }
static void pneu(bool on) { manda(CMD_PNEU, on); }
static void estouros(bool on) { manda(CMD_ESTOUROS, on); }
static void simular(float kmh) { manda(CMD_SIMULAR, 0, kmh); }
// dado da roda: velocidade (km/h), corrente (A), hora em ms em que chegou
static void roda(float kmh, float amp, uint32_t quandoMs) { rodaV = kmh; rodaI = amp; rodaT = quandoMs; }

static void tarefa(void*) {
  static int16_t mono[256];
  static int32_t est[256 * 2];
  uint32_t silencio = 0, tCpu = 0, tTotal = 0, tMed = millis();
  bool amp = false;
  for (;;) {
    Cmd c;
    while (xQueueReceive(fila, &c, 0) == pdTRUE) {
      switch (c.tipo) {
        case CMD_LIGAR: mesa.ligar(c.a != 0); break;
        case CMD_MOTOR: mesa.escolher(c.a); break;
        case CMD_VOLUME: mesa.volume = clampf(c.a / 100.0f, 0, 1); break;
        case CMD_BUZ_APERTA: mesa.apertaBuzina(c.a); break;
        case CMD_BUZ_SOLTA: mesa.soltaBuzina(); break;
        case CMD_PNEU: mesa.pneuOn = c.a != 0; break;
        case CMD_ESTOUROS: mesa.estouros = c.a != 0; break;
        case CMD_SIMULAR: mesa.simAlvo = c.f; break;
      }
    }
    // velocidade da roda: so vale se chegou nos ultimos 1,5 s
    uint32_t t = rodaT; float vr = NAN, ir = NAN;
    if (t != 0 && millis() - t < 1500) { vr = rodaV; ir = rodaI; }
    uint32_t t0 = micros();
    bool toca = mesa.tocando();
    if (toca) mesa.gera(mono, 256, vr, ir, t);
    else memset(mono, 0, sizeof(mono));
    tCpu += micros() - t0;
    for (int i = 0; i < 256; i++) { int32_t s = (int32_t)mono[i] << 16; est[i * 2] = s; est[i * 2 + 1] = s; }
    // amplificador: liga junto com o som, desliga depois de 2 s de silencio
    if (toca) { silencio = 0; if (!amp && pinoAmp >= 0) { digitalWrite(pinoAmp, HIGH); amp = true; } }
    else if (amp && (silencio += 256) > (uint32_t)SR * 2) { if (pinoAmp >= 0) digitalWrite(pinoAmp, LOW); amp = false; }
    size_t esc = 0;
    i2s_channel_write(tx, est, sizeof(est), &esc, portMAX_DELAY);
    estLigado = mesa.ligado; estTocando = toca; estMotor = mesa.motorId; estVol = (uint8_t)lrintf(mesa.volume * 100);
    estPneu = mesa.pneuOn; estV = mesa.v; estRpm = mesa.rpm; estMarcha = mesa.marcha;
    if (millis() - tMed > 2000) { tTotal = (millis() - tMed) * 1000; estCarga = tCpu * 100 / tTotal; tCpu = 0; tMed = millis(); }
  }
}

static bool begin(int pBck, int pDin, int pLck, int pAmp) {
  pinoAmp = pAmp;
  if (pAmp >= 0) { pinMode(pAmp, OUTPUT); digitalWrite(pAmp, LOW); }
  mesa.init();
  fila = xQueueCreate(16, sizeof(Cmd));
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.auto_clear = true; cc.dma_desc_num = 6; cc.dma_frame_num = 256;
  if (i2s_new_channel(&cc, &tx, nullptr) != ESP_OK) return false;
  i2s_std_config_t sc = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SR),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = (gpio_num_t)pBck, .ws = (gpio_num_t)pLck, .dout = (gpio_num_t)pDin,
                  .din = I2S_GPIO_UNUSED, .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false } },
  };
  if (i2s_channel_init_std_mode(tx, &sc) != ESP_OK) return false;
  if (i2s_channel_enable(tx) != ESP_OK) return false;
  xTaskCreate(tarefa, "som", 6144, nullptr, 5, nullptr);
  return true;
}
#endif

}  // namespace som
