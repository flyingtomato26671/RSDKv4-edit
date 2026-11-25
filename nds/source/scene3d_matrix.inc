void SetIdentityMatrix(Matrix *matrix)
{
    matrix->values[0][0] = 0x100;
    matrix->values[0][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[0][3] = 0;

    matrix->values[1][0] = 0;
    matrix->values[1][1] = 0x100;
    matrix->values[1][2] = 0;
    matrix->values[1][3] = 0;

    matrix->values[2][0] = 0;
    matrix->values[2][1] = 0;
    matrix->values[2][2] = 0x100;
    matrix->values[2][3] = 0;

    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void MatrixMultiply(Matrix *matrixA, Matrix *matrixB)
{
#if RETRO_PLATFORM == RETRO_PS1 && PS1_GAME != 1 && !defined(RETRO_PS1_HOST_TOOL)
    // a row of A in registers, written back once its four entries are done (each output row reads only its own A row):
    // upstream's terms in upstream's order, without the loop's i / 4, i % 4 and the output copy (docs/43: several calls a
    // frame in the special stage). A == B (in place) keeps upstream's loop below.
    if (matrixA != matrixB) {
        const int(*b)[4] = matrixB->values;
        for (int r = 0; r < 4; ++r) {
            int *a = matrixA->values[r];
            const int a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
            const int o0 = (a3 * b[3][0] >> 8) + (a2 * b[2][0] >> 8) + (a1 * b[1][0] >> 8) + (a0 * b[0][0] >> 8);
            const int o1 = (a3 * b[3][1] >> 8) + (a2 * b[2][1] >> 8) + (a1 * b[1][1] >> 8) + (a0 * b[0][1] >> 8);
            const int o2 = (a3 * b[3][2] >> 8) + (a2 * b[2][2] >> 8) + (a1 * b[1][2] >> 8) + (a0 * b[0][2] >> 8);
            const int o3 = (a3 * b[3][3] >> 8) + (a2 * b[2][3] >> 8) + (a1 * b[1][3] >> 8) + (a0 * b[0][3] >> 8);
            a[0] = o0, a[1] = o1, a[2] = o2, a[3] = o3;
        }
        return;
    }
#endif
    int output[16];

    for (int i = 0; i < 0x10; ++i) {
        uint rowA = i / 4;
        uint rowB = i % 4;
        output[i] = (matrixA->values[rowA][3] * matrixB->values[3][rowB] >> 8) + (matrixA->values[rowA][2] * matrixB->values[2][rowB] >> 8)
                    + (matrixA->values[rowA][1] * matrixB->values[1][rowB] >> 8) + (matrixA->values[rowA][0] * matrixB->values[0][rowB] >> 8);
    }

    for (int i = 0; i < 0x10; ++i) matrixA->values[i / 4][i % 4] = output[i];
}
void MatrixTranslateXYZ(Matrix *matrix, int XPos, int YPos, int ZPos)
{
    matrix->values[0][0] = 0x100;
    matrix->values[0][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[0][3] = 0;

    matrix->values[1][0] = 0;
    matrix->values[1][1] = 0x100;
    matrix->values[1][2] = 0;
    matrix->values[1][3] = 0;

    matrix->values[2][0] = 0;
    matrix->values[2][1] = 0;
    matrix->values[2][2] = 0x100;
    matrix->values[2][3] = 0;

    matrix->values[3][0] = XPos;
    matrix->values[3][1] = YPos;
    matrix->values[3][2] = ZPos;
    matrix->values[3][3] = 0x100;
}
void MatrixScaleXYZ(Matrix *matrix, int scaleX, int scaleY, int scaleZ)
{
    matrix->values[0][0] = scaleX;
    matrix->values[0][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[0][3] = 0;

    matrix->values[1][0] = 0;
    matrix->values[1][1] = scaleY;
    matrix->values[1][2] = 0;
    matrix->values[1][3] = 0;

    matrix->values[2][0] = 0;
    matrix->values[2][1] = 0;
    matrix->values[2][2] = scaleZ;
    matrix->values[2][3] = 0;

    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void MatrixRotateX(Matrix *matrix, int rotationX)
{
    int sine   = sin512LookupTable[rotationX & 0x1FF] >> 1;
    int cosine = cos512LookupTable[rotationX & 0x1FF] >> 1;

    matrix->values[0][0] = 0x100;
    matrix->values[0][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[0][3] = 0;

    matrix->values[1][0] = 0;
    matrix->values[1][1] = cosine;
    matrix->values[1][2] = sine;
    matrix->values[1][3] = 0;

    matrix->values[2][0] = 0;
    matrix->values[2][1] = -sine;
    matrix->values[2][2] = cosine;
    matrix->values[2][3] = 0;

    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void MatrixRotateY(Matrix *matrix, int rotationY)
{
    int sine   = sin512LookupTable[rotationY & 0x1FF] >> 1;
    int cosine = cos512LookupTable[rotationY & 0x1FF] >> 1;

    matrix->values[0][0] = cosine;
    matrix->values[0][1] = 0;
    matrix->values[0][2] = sine;
    matrix->values[0][3] = 0;

    matrix->values[1][0] = 0;
    matrix->values[1][1] = 0x100;
    matrix->values[1][2] = 0;
    matrix->values[1][3] = 0;

    matrix->values[2][0] = -sine;
    matrix->values[2][1] = 0;
    matrix->values[2][2] = cosine;
    matrix->values[2][3] = 0;

    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void MatrixRotateZ(Matrix *matrix, int rotationZ)
{
    int sine             = sin512LookupTable[rotationZ & 0x1FF] >> 1;
    int cosine           = cos512LookupTable[rotationZ & 0x1FF] >> 1;
    matrix->values[0][0] = cosine;
    matrix->values[0][1] = 0;
    matrix->values[0][2] = sine;
    matrix->values[0][3] = 0;

    matrix->values[1][0] = 0;
    matrix->values[1][1] = 0x100;
    matrix->values[1][2] = 0;
    matrix->values[1][3] = 0;

    matrix->values[2][0] = -sine;
    matrix->values[2][1] = 0;
    matrix->values[2][2] = cosine;
    matrix->values[2][3] = 0;

    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void MatrixRotateXYZ(Matrix *matrix, short rotationX, short rotationY, short rotationZ)
{
    int sinX = sin512LookupTable[rotationX & 0x1FF] >> 1;
    int cosX = cos512LookupTable[rotationX & 0x1FF] >> 1;
    int sinY = sin512LookupTable[rotationY & 0x1FF] >> 1;
    int cosY = cos512LookupTable[rotationY & 0x1FF] >> 1;
    int sinZ = sin512LookupTable[rotationZ & 0x1FF] >> 1;
    int cosZ = cos512LookupTable[rotationZ & 0x1FF] >> 1;

    matrix->values[0][0] = (cosZ * cosY >> 8) + (sinZ * (sinY * sinX >> 8) >> 8);
    matrix->values[0][1] = (sinZ * cosY >> 8) - (cosZ * (sinY * sinX >> 8) >> 8);
    matrix->values[0][2] = sinY * cosX >> 8;
    matrix->values[0][3] = 0;

    matrix->values[1][0] = sinZ * -cosX >> 8;
    matrix->values[1][1] = cosZ * cosX >> 8;
    matrix->values[1][2] = sinX;
    matrix->values[1][3] = 0;

    matrix->values[2][0] = (sinZ * (cosY * sinX >> 8) >> 8) - (cosZ * sinY >> 8);
    matrix->values[2][1] = (sinZ * -sinY >> 8) - (cosZ * (cosY * sinX >> 8) >> 8);
    matrix->values[2][2] = cosY * cosX >> 8;
    matrix->values[2][3] = 0;

    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
#if !RETRO_REV00
#if RETRO_PLATFORM == RETRO_PS1 || defined(PS1_INVERSE_UNDER_TEST)
// PS1 (docs/30 phase 7.2b): upstream's MatrixInverse below works in doubles (soft-float here: hundreds of library calls,
// every frame for the special stage's view matrix). For a matrix with last column 0 0 0 and translation 0 (256 in the
// corner: a rotation, as MatrixRotateXYZ makes) all of its products and sums are exact (integers over powers of two)
// until 1.0 / det: integer cofactors, then the same double operations -> the same bits (tools/scene3d/inverse_check.sh,
// 3,000,000 cases). Anything else takes upstream's code.
// The doubles below in integers (docs/43: ~30 soft-float calls an inverse, every frame): RN(2^24 / A) for A > 0 as M * 2^ex
// (M in [2^52, 2^53), round to nearest even, as the soft-float division), by restoring long division.
typedef unsigned long long PS1U64;
#if PS1_GAME != 1 // Sonic 2 (Sonic 1's EXE stays as it was: its inverses keep the doubles)
static int PS1BitLen64(PS1U64 v)
{
    int n = 0;
    if (v >> 32) n += 32, v >>= 32;
    uint32_t w = (uint32_t)v;
    if (w >> 16) n += 16, w >>= 16;
    if (w >> 8) n += 8, w >>= 8;
    if (w >> 4) n += 4, w >>= 4;
    if (w >> 2) n += 2, w >>= 2;
    if (w >> 1) n += 1, w >>= 1;
    return n + (int)w;
}
static void PS1RecipRN(PS1U64 A, PS1U64 &M, int &ex)
{
    const int k = PS1BitLen64(A); // A in [2^(k-1), 2^k)
    if (A == 1ull << (k - 1)) {
        M = 1ull << 52, ex = 24 - (52 + k - 1);
        return;
    }
    PS1U64 q = 1, r = (1ull << k) - A; // 2^k / A: 1, remainder 2^k - A
    for (int j = 0; j < 52; ++j) {
        r <<= 1, q <<= 1;
        if (r >= A)
            r -= A, q |= 1;
    }
    ex = 24 - (52 + k); // q = floor(2^(52 + k) / A) in (2^52, 2^53)
    r <<= 1;
    if (r > A || (r == A && (q & 1)))
        if (++q == 1ull << 53)
            q >>= 1, ++ex;
    M = q;
}
// trunc(RN(X * M * 2^ex) * 2^-drop) for X != 0 (the product rounded to 53 bits as a double, then (int) of the value scaled
// by a power of two); false when the result would not fit an int (the caller's double code decides then).
static bool PS1MulRNTrunc(long long X, PS1U64 M, int ex, int drop, bool negDet, int &out)
{
    const bool neg = (X < 0) != negDet;
    const PS1U64 ax = X < 0 ? -(PS1U64)X : (PS1U64)X; // < 2^50
    const uint32_t x0 = (uint32_t)ax, x1 = (uint32_t)(ax >> 32), m0 = (uint32_t)M, m1 = (uint32_t)(M >> 32);
    const PS1U64 lo = (PS1U64)x0 * m0, mid = (PS1U64)x0 * m1 + (PS1U64)x1 * m0;
    PS1U64 plo = lo + (mid << 32);
    PS1U64 phi = (PS1U64)x1 * m1 + (mid >> 32) + (plo < lo);
    const int b = phi ? 64 + PS1BitLen64(phi) : PS1BitLen64(plo);
    PS1U64 m;
    int sh = 0;
    if (b > 53) {
        sh = b - 53; // <= 49
        m = (plo >> sh) | (phi << (64 - sh));
        const PS1U64 rem = plo & ((1ull << sh) - 1), half = 1ull << (sh - 1);
        if (rem > half || (rem == half && (m & 1)))
            if (++m == 1ull << 53)
                m >>= 1, ++sh;
    }
    else {
        m = plo;
    }
    const int e = sh + ex - drop;
    PS1U64 v;
    if (e >= 0) {
        if (e > 30 || (m << e) >> 31)
            return false;
        v = m << e;
    }
    else {
        v = -e >= 64 ? 0 : m >> -e;
        if (v >> 31)
            return false;
    }
    out = neg ? -(int)v : (int)v;
    return true;
}
#endif
PS1_S2_SMALL static bool PS1MatrixInverseRotation(Matrix *matrix)
{
    int(*v)[4] = matrix->values;
    if (v[0][3] || v[1][3] || v[2][3] || v[3][0] || v[3][1] || v[3][2] || v[3][3] != 256)
        return false;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            if (v[y][x] > 32767 || v[y][x] < -32767)
                return false;
    typedef long long i64;
    const i64 a = v[0][0], b = v[0][1], c = v[0][2], d = v[1][0], e = v[1][1], f = v[1][2], g = v[2][0], h = v[2][1], k = v[2][2];
    // 2x2 cofactors (upstream inv[] with this structure's zero terms: exact doubles, over 2^16)
    i64 N[16] = {};
    N[0] = e * k - h * f, N[4] = -d * k + g * f, N[8] = d * h - g * e;
    N[1] = -b * k + h * c, N[5] = a * k - g * c, N[9] = -a * h + g * b;
    N[2] = b * f - e * c, N[6] = -a * f + d * c, N[10] = a * e - d * b;
    const i64 D = a * N[0] + b * N[4] + c * N[8]; // det * 2^24 (upstream's det: exact)
    if (D == 0)
        return true; // upstream returns: matrix unchanged
    const i64 D15 = a * e * k - a * f * h - d * b * k + d * c * h + g * b * f - g * c * e; // inv[15] * 2^24
#if PS1_GAME != 1
    // det = 1.0 / (D / 2^24) = RN(2^24 / D); each entry (int)(((X / 2^s) * det) * 256) = trunc(RN(X * det) / 2^(s - 8))
    // (scalings by powers of two are exact): in integers, the doubles only where the result would not fit an int
    PS1U64 M;
    int ex;
    PS1RecipRN(D < 0 ? -(PS1U64)D : (PS1U64)D, M, ex);
    int out[16];
    for (int i = 0; i < 16; ++i) {
        const i64 X = i == 15 ? D15 : N[i];
        if (!X) { // 0.0 * det * 256 truncates to 0 (det is finite): N[3], [7], [11]-[14] always, the others when an entry is
            out[i] = 0;
            continue;
        }
        if (PS1MulRNTrunc(X, M, ex, i == 15 ? 16 : 8, D < 0, out[i]))
            continue;
        double det = 1.0 / ((double)D / 16777216.0);
        double inv = i == 15 ? (double)D15 / 16777216.0 : (double)N[i] / 65536.0;
        out[i]     = (int)((inv * det) * 256);
    }
#else
    double det = 1.0 / ((double)D / 16777216.0);
    int out[16];
    for (int i = 0; i < 16; ++i) {
        if (i != 15 && !N[i]) { // 0.0 * det * 256 truncates to 0 (det is finite): no soft-float calls (docs/43 C12;
            out[i] = 0;         // N[3], [7], [11]-[14] are always 0 here, the others when an entry is)
            continue;
        }
        double inv = i == 15 ? (double)D15 / 16777216.0 : (double)N[i] / 65536.0;
        out[i]     = (int)((inv * det) * 256);
    }
#endif
    for (int i = 0; i < 16; ++i) v[i / 4][i % 4] = out[i];
    return true;
}
#endif
#if RETRO_PLATFORM == RETRO_PS1 && PS1_GAME != 1 && !defined(RETRO_PS1_HOST_TOOL)
#define PS1_INV_TABLE 1
// Sonic 2: upstream's 16 cofactors (MatrixInverse below) as a table: each one's six terms m[a] * m[b] * m[c] in
// upstream's order, the first negated or not, the others added or subtracted (bit k of s_ps1InvNeg: term k has a minus):
// the same double operations in the same order, so the same bits (tools/scene3d/inverse_check.sh). Unrolled they were
// 6.3 KB of soft-float calls, for the inverses that are not rotations (none in the special stage): RAM (docs/43).
static const uint8_t s_ps1InvTerm[16][6][3] = {
    {{5, 10, 15}, {5, 11, 14}, {9, 6, 15}, {9, 7, 14}, {13, 6, 11}, {13, 7, 10}}, // inv[0]
    {{1, 10, 15}, {1, 11, 14}, {9, 2, 15}, {9, 3, 14}, {13, 2, 11}, {13, 3, 10}}, // inv[1]
    {{1, 6, 15}, {1, 7, 14}, {5, 2, 15}, {5, 3, 14}, {13, 2, 7}, {13, 3, 6}}, // inv[2]
    {{1, 6, 11}, {1, 7, 10}, {5, 2, 11}, {5, 3, 10}, {9, 2, 7}, {9, 3, 6}}, // inv[3]
    {{4, 10, 15}, {4, 11, 14}, {8, 6, 15}, {8, 7, 14}, {12, 6, 11}, {12, 7, 10}}, // inv[4]
    {{0, 10, 15}, {0, 11, 14}, {8, 2, 15}, {8, 3, 14}, {12, 2, 11}, {12, 3, 10}}, // inv[5]
    {{0, 6, 15}, {0, 7, 14}, {4, 2, 15}, {4, 3, 14}, {12, 2, 7}, {12, 3, 6}}, // inv[6]
    {{0, 6, 11}, {0, 7, 10}, {4, 2, 11}, {4, 3, 10}, {8, 2, 7}, {8, 3, 6}}, // inv[7]
    {{4, 9, 15}, {4, 11, 13}, {8, 5, 15}, {8, 7, 13}, {12, 5, 11}, {12, 7, 9}}, // inv[8]
    {{0, 9, 15}, {0, 11, 13}, {8, 1, 15}, {8, 3, 13}, {12, 1, 11}, {12, 3, 9}}, // inv[9]
    {{0, 5, 15}, {0, 7, 13}, {4, 1, 15}, {4, 3, 13}, {12, 1, 7}, {12, 3, 5}}, // inv[10]
    {{0, 5, 11}, {0, 7, 9}, {4, 1, 11}, {4, 3, 9}, {8, 1, 7}, {8, 3, 5}}, // inv[11]
    {{4, 9, 14}, {4, 10, 13}, {8, 5, 14}, {8, 6, 13}, {12, 5, 10}, {12, 6, 9}}, // inv[12]
    {{0, 9, 14}, {0, 10, 13}, {8, 1, 14}, {8, 2, 13}, {12, 1, 10}, {12, 2, 9}}, // inv[13]
    {{0, 5, 14}, {0, 6, 13}, {4, 1, 14}, {4, 2, 13}, {12, 1, 6}, {12, 2, 5}}, // inv[14]
    {{0, 5, 10}, {0, 6, 9}, {4, 1, 10}, {4, 2, 9}, {8, 1, 6}, {8, 2, 5}}, // inv[15]
};
static const uint8_t s_ps1InvNeg[16] = {0x26, 0x19, 0x26, 0x19, 0x19, 0x26, 0x19, 0x26, 0x26, 0x19, 0x26, 0x19, 0x19, 0x26, 0x19, 0x26};
PS1_S2_SMALL static void PS1MatrixInverseTable(Matrix *matrix)
{
    double inv[16], det;
    double m[16];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            m[(y << 2) + x] = matrix->values[y][x] / 256.0;
        }
    }
    for (int i = 0; i < 16; ++i) {
        const uint8_t(*t)[3] = s_ps1InvTerm[i];
        const int neg        = s_ps1InvNeg[i];
        double acc           = (neg & 1 ? -m[t[0][0]] : m[t[0][0]]) * m[t[0][1]] * m[t[0][2]];
        for (int k = 1; k < 6; ++k) {
            const double p = m[t[k][0]] * m[t[k][1]] * m[t[k][2]];
            acc            = (neg >> k & 1) ? acc - p : acc + p;
        }
        inv[i] = acc;
    }

    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

    if (det == 0)
        return;

    det = 1.0 / det;

    for (int i = 0; i < 0x10; ++i) inv[i] = (int)((inv[i] * det) * 256);
    for (int i = 0; i < 0x10; ++i) matrix->values[i / 4][i % 4] = inv[i];
}
#endif
// -Os (docs/43: RAM): its double fallback was 8.5 KB at -O2; the special stage's inverses take the rotation path above
PS1_S2_SMALL void MatrixInverse(Matrix *matrix)
{
#if RETRO_PLATFORM == RETRO_PS1
    if (PS1MatrixInverseRotation(matrix))
        return;
#endif
#ifdef PS1_INV_TABLE
    PS1MatrixInverseTable(matrix);
#else
    double inv[16], det;
    double m[16];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            m[(y << 2) + x] = matrix->values[y][x] / 256.0;
        }
    }

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];

    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];

    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];

    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];

    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];

    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];

    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];

    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];

    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];

    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];

    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];

    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];

    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];

    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];

    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];

    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

    if (det == 0)
        return;

    det = 1.0 / det;

    for (int i = 0; i < 0x10; ++i) inv[i] = (int)((inv[i] * det) * 256);
    for (int i = 0; i < 0x10; ++i) matrix->values[i / 4][i % 4] = inv[i];
#endif
}
#endif
