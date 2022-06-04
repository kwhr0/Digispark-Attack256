#define BRIGHTNESS    4
#define BRIGHTNESS_I  16
#define XN            16
#define YN            16
#define CAND_N        80
#define WAIT          250     // mS
#define SYSCLK        16500   // kHz

typedef signed char s8;
typedef unsigned char u8;
typedef unsigned short u16;

struct Pos {
  s8 x, y;
};

static const Pos ofstbl[] PROGMEM = {
  { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 },
  { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 }
};

static const u16 tonetbl[] PROGMEM = {
  //  8, 9, 9, 10, 10, 11, 12, 12, 13, 14, 15, 16,
  //  17, 18, 19, 20, 21, 22, 24, 25, 26, 28, 30, 31,
  //  33, 35, 37, 40, 42, 44, 47, 50, 53, 56, 59, 63,
  67, 70, 75, 79, 84, 89, 94, 100, 106, 112, 118, 126,
  133, 141, 149, 158, 168, 178, 188, 199, 211, 224, 237, 251,
  266, 282, 299, 316, 335, 355, 376, 399, 422, 447, 474, 502,
  532, 564, 597, 633, 670, 710, 752, 797, 845, 895, 948, 1004,
  1064, 1127, 1194, 1265, 1341, 1420, 1505, 1594, 1689, 1790, 1896, 2009,
  2128, 2255, 2389, 2531, 2681, 2841, 3010, 3189, 3378, 3579, 3792, 4017,
  //  4256, 4509, 4778, 5062, 5363, 5682, 6019, 6377, 6757, 7158, 7584, 8035,
  //  8513, 9019, 9555, 10123, 10725, 11363, 12039, 12755,
};

static const u8 wkey[] PROGMEM = {
  0, 2, 4, 5, 7, 9, 11,
  12, 14, 16, 17, 19, 21, 23,
  24, 26, 28, 29, 31, 33, 35,
  36, 38, 40, 41, 43, 45, 47,
  48, 50, 52, 53, 55, 57, 59,
};

static u8 vram[YN][XN];
static u8 cand[CAND_N];
static u8 *candp;
static u8 score[8];
static u8 pwm, posX, posY, curColor;
volatile static u8 intcnt;

struct Op {
  static const int N = 3;
  void NoteOn(int note, int _rate) {
    cnt = 0;
    delta = pgm_read_word(&tonetbl[note]);
    env = 0x7fff;
    rate = _rate;
  }
  void NoteOff() {
    env = 0;
  }
  int Update() {
    int r = 0;
    if (env) {
      r = ((cnt += delta) >= 0 ? env : -env) >> 8;
      if ((env -= rate) < 0) env = 0;
    }
    return r;
  }
  int cnt, delta, env, rate;
};

static Op op[Op::N];

static void cls() {
  memset(vram, 0, sizeof(vram));
  digitalWrite(1, LOW);
}

static inline void __attribute__((naked)) sendbyte(u8 v) {
  u8 i;
  // T0H: 7clocks 424nS
  // T1H: 13clocks 788nS
  // T0H+T0L=T1H+T1L=21clocks 1272nS
  asm volatile(
    "ldi %[i],8\n"
    "cli\n"
    "label1:\n"
    "sbi %[portb],2\n"
    "lsl %[v]\n"
    "brcc bit_zero\n"
    "nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"
    "cbi %[portb],2\n"
    "nop\n"
    "rjmp label2\n"
    "bit_zero:\n"
    "nop\nnop\n"
    "cbi %[portb],2\n"
    "nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"
    "label2:\n"
    "dec %[i]\n"
    "brne label1\n"
    "sei\n"
    : [i] "+r" (i), [v] "+r" (v)
    : [portb] "I" (_SFR_IO_ADDR(PORTB))
  );
}

static void draw() {
  for (int y = YN - 1; y >= 0; y--)
    for (int x = 0; x < XN; x++) {
      int d = vram[y][y & 1 ? x : XN - 1 - x];
      int v = d & 8 ? BRIGHTNESS_I : BRIGHTNESS;
      sendbyte(d & 4 ? v : 0);
      sendbyte(d & 2 ? v : 0);
      sendbyte(d & 1 ? v : 0);
    }
}

static void wait(int duration) {
  for (int i = 0; i < WAIT * duration; i++)
    for (int j = 0; j < SYSCLK / 256; j++) { // 1mS loop
      int t = intcnt;
      int acc = 0x80 + op[0].Update() + op[1].Update() + op[2].Update();
      if (acc < 0) acc = 0;
      else if (acc > 0xff) acc = 0xff;
      while (intcnt == t)
        ;
      pwm = acc;
    }
}

static void dispwait(int duration) {
  draw();
  wait(duration);
}

static int getv(int x, int y, int dir = 0, int len = 0) {
  const Pos *p = &ofstbl[dir & 7];
  x += len * (s8)pgm_read_byte(&p->x);
  y += len * (s8)pgm_read_byte(&p->y);
  return x >= 0 && x < XN && y >= 0 && y < YN ? vram[y][x] & 0xf : -1;
}

static int setv(int x, int y, int v, int dir = 0, int len = 0) {
  const Pos *p = &ofstbl[dir & 7];
  x += len * (s8)pgm_read_byte(&p->x);
  y += len * (s8)pgm_read_byte(&p->y);
  if (x >= 0 && x < XN && y >= 0 && y < YN) {
    u8 *p = &vram[y][x];
    *p = *p & 0xf0 | v & 0xf;
  }
}

// 表示レイヤー(下位4ビット)から保存レイヤー(上位4ビット)にコピー

static void copylayer() {
  u8 *p = (u8 *)vram, *lim = (u8 *)vram + sizeof(vram);
  for (; p < lim; p++) *p |= *p << 4;
}

// 表示レイヤー:funcが0を返す場合は1つ下の行の保存レイヤーをコピー、そうでない場合は保持
// 保存レイヤー:常に1つ下の行の保存レイヤーをコピー

template <typename F> void scroll(F func) {
  u8 *sp = (u8 *)vram + XN, *dp = (u8 *)vram;
  for (int y = 0; y < YN - 1; y++)
    for (int x = 0; x < XN; x++) {
      *dp = *sp & 0xf0 | (func(x, y) ? *dp & 0xf : *sp >> 4);
      sp++;
      dp++;
    }
  memset(dp, 0, XN);
}

static int selcolor() {
  return random(1, 8);
}

static void noteon(int index, int note, int rate) {
  if (index >= 0 && index < Op::N) op[index].NoteOn(note, rate);
}

static void se3(int c) {
  int k = pgm_read_byte(&wkey[c + 14 - 1]);
  noteon(0, k, 1);
  noteon(1, k + 4, 2);
  noteon(2, k + 7, 3);
}

static void se3b(int x) {
  int k = pgm_read_byte(&wkey[x + 7]);
  noteon(0, k, 1);
  noteon(1, k + 12, 1);
  noteon(2, k + 24, 1);
}

static void append_cand(int x, int y) {
  if (candp < cand + CAND_N) *candp++ = x << 4 | y;
  else digitalWrite(1, HIGH);
}

static void sandwich(int c, bool f) {
  int x, y, d, i, c1;
  for (y = 0; y < YN; y++)
    for (x = 0; x < XN; x++) {
      if (getv(x, y)) continue;
      for (d = 0; d < 8; d++) {
        for (i = 1; (c1 = getv(x, y, d, i)) > 0 && c1 != c; i++)
          ;
        if (i > 1 && (f ? c1 > 0 : c1 == 0)) {
          append_cand(x, y);
          break;
        }
      }
    }
}

static bool process() {
  int i, x, y, d, c = selcolor(), c1;
  // ひっくり返せるところを探す
  candp = cand;
  sandwich(c, true);
  // ひっくり返せるところがなければ、次も自分のターンだった場合にひっくり返せる場所を探す
  if (candp == cand) sandwich(c, false);
  // それもなければ、隣接するところを探す
  if (candp == cand)
    for (y = 0; y < YN; y++)
      for (x = 0; x < XN; x++) {
        if (getv(x, y)) continue;
        for (d = 0; d < 8; d++)
          if (getv(x, y, d, 1) > 0) {
            append_cand(x, y);
            break;
          }
      }
  // 置けるところがなければ終了
  if (candp == cand) return false;
  // ランダムで1箇所選ぶ
  u8 t = cand[random(candp - cand)];
  x = t >> 4;
  y = t & 15;
  // 挟む相手を記録
  u8 len[8] = { 0 };
  for (d = 0; d < 8; d++) {
    for (i = 1; (c1 = getv(x, y, d, i)) > 0 && c1 != c; i++)
      ;
    if (i > 1 && c1 > 0) len[d] = i;
  }
  // 挟む相手を強調、置く石を点滅
  for (d = 0; d < 8; d++)
    if (len[d]) setv(x, y, c | 8, d, len[d]);
  for (i = 0; i < 3; i++) {
    se3(c);
    setv(x, y, c | 8);
    dispwait(1);
    setv(x, y, 0);
    dispwait(1);
  }
  se3(c);
  setv(x, y, c);
  dispwait(1);
  for (d = 0; d < 8; d++)
    if (len[d]) setv(x, y, c, d, len[d]);
  // 挟んだ石をひっくり返す
  for (d = 0; d < 8; d++) {
    for (i = 1; (c1 = getv(x, y, d, i)) > 0 && c1 != c; i++)
      ;
    if (i > 1 && c1 > 0)
      for (int j = 1; j < i; j++) {
        se3(c + (c < 5 ? j : -j));
        setv(x, y, c, d, j);
        dispwait(1);
      }
  }
  return true;
}

static bool sortstep(int x, int y, int &c) {
  int xs = x, ys = y;
  do if (++xs >= XN) {
      xs = 0;
      ys++;
    }
  while (ys < YN && getv(xs, ys) != c);
  if (ys < YN) {
    int c1 = getv(x, y);
    setv(x, y, c);
    setv(xs, ys, c1);
    se3(c1);
    dispwait(1);
    return false;
  }
  else c++;
  return true;
}

static void sort() {
  int x, y, c = 1;
  for (y = 0; y < YN; y++)
    for (x = 0; x < XN; x++)
      while (getv(x, y) != c && sortstep(x, y, c))
        ;
}

static void putch(int c) {
  static const int font[] PROGMEM = {
    075557, 011111, 071747, 071717, 055711,
    074717, 074757, 071111, 075757, 075717,
  };
  int x, y, f = isdigit(c) ? pgm_read_word(&font[c - '0']) : 0;
  for (y = posY + 4; y >= posY; y--) {
    for (x = posX + 3; x > posX; x--) {
      setv(x, y, f & 1 ? curColor : 0);
      f >>= 1;
    }
    setv(x, y, 0);
  }
  posX += 4;
}

static void putu(int v, int n) {
  u8 c[6];
  u8 *p = &c[sizeof(c) - 1];
  *p = 0;
  do {
    *--p = v % 10 + '0';
    n--;
  } while (v /= 10);
  while (n-- > 0) putch('0');
  while (*p) putch(*p++);
}

static void countall() {
  const int X = 4, Y = 11;
  curColor = 1;
  memset(score, 0, sizeof(score));
  copylayer();
  for (int y = 0; y < YN; y++) {
    for (int x = 0; x < XN; x++) {
      bool f;
      do {
        f = (getv(x, 0)) == curColor;
        if (f) {
          setv(x, 0, 0);
          score[curColor]++;
          se3(curColor);
        }
        posX = X;
        posY = Y;
        putu(score[curColor], 3);
        dispwait(f ? 1 : 4);
        if (!f && ++curColor >= sizeof(score)) return;
      } while (!f);
    }
    if (y < YN - 1) scroll(+[](int x1, int y1) { return x1 >= X && y1 >= Y; });
  }
  dispwait(4);
}

static void dispscore() {
  posY = 0;
  for (int i = 0; i < 3; i++) {
    int m = 0;
    for (int j = 0; j < 8; j++)
      if (m < score[j]) {
        m = score[j];
        curColor = j;
      }
    posX = 4;
    putu(score[curColor], 3);
    score[curColor] = 0;
    posY += 5;
    se3b(2 * i);
    dispwait(4);
  }
  se3b(7);
}

ISR(TIMER0_OVF_vect) {
  intcnt++;
  OCR0A = pwm; // PB0
}

void setup() {
  randomSeed(analogRead(2) << 10 | analogRead(2));
  pinMode(0, OUTPUT); // sound
  pinMode(1, OUTPUT); // onboard LED
  pinMode(2, OUTPUT); // WS2812B
  TCCR0A = 0x83; // COM0A=2, Fast PWM
  TCCR0B = 1; // WGM02=0, no prescaling
  TIMSK = 2; // TOIE0=1
}

void loop() {
  cls();
  dispwait(4);
  setv(XN / 2,     YN / 2,     selcolor());
  setv(XN / 2 - 1, YN / 2,     selcolor());
  setv(XN / 2,     YN / 2 - 1, selcolor());
  setv(XN / 2 - 1, YN / 2 - 1, selcolor());
  dispwait(4);
  while (process())
    dispwait(4);
  dispwait(12);
  sort();
  dispwait(12);
  countall();
  cls();
  dispscore();
  dispwait(12);
}
