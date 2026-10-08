/* D3DX maths Fallout: New Vegas imports (row-major D3DXMATRIX, row vectors: v' = v M), the shader-profile queries, and
 * "not yet" answers for the shader assembler/compiler and image helpers the game may use outside normal play.
 * Every function returns its output pointer, as D3DX does. */
#include "w32.h"

typedef struct { float m[4][4]; } Mat;
static Mat ldm(uint32_t a) { Mat r; memcpy(&r, GP(a), 64); return r; }
static void stm(uint32_t a, const Mat *m) { memcpy(GP(a), m, 64); }
static void ldv(uint32_t a, float *v, int n) { memcpy(v, GP(a), 4 * (size_t)n); }
static void stv(uint32_t a, const float *v, int n) { memcpy(GP(a), v, 4 * (size_t)n); }
static Mat mul(const Mat *a, const Mat *b)
{
    Mat r;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++)
        r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j] + a->m[i][3] * b->m[3][j];
    return r;
}
static Mat transpose(const Mat *a) { Mat r; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r.m[i][j] = a->m[j][i]; return r; }

IMPL(d3dx9_29, D3DXMatrixMultiply) { Mat a = ldm(ARG(1)), b = ldm(ARG(2)), r = mul(&a, &b); stm(ARG(0), &r); RET(ARG(0), 3); }
IMPL(d3dx9_29, D3DXMatrixMultiplyTranspose) { Mat a = ldm(ARG(1)), b = ldm(ARG(2)), r = mul(&a, &b), t = transpose(&r); stm(ARG(0), &t); RET(ARG(0), 3); }
IMPL(d3dx9_29, D3DXMatrixTranspose) { Mat a = ldm(ARG(1)), t = transpose(&a); stm(ARG(0), &t); RET(ARG(0), 2); }
IMPL(d3dx9_29, D3DXMatrixInverse)                     /* (out, float *determinant, m): NULL when singular */
{
    Mat a = ldm(ARG(2)); double inv[16], m[16];
    for (int k = 0; k < 16; k++) m[k] = a.m[k / 4][k % 4];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (ARG(1)) { float d = (float)det; memcpy(GP(ARG(1)), &d, 4); }
    if (det == 0) RET(0, 3);
    Mat r; for (int k = 0; k < 16; k++) r.m[k / 4][k % 4] = (float)(inv[k] / det);
    stm(ARG(0), &r); RET(ARG(0), 3);
}
IMPL(d3dx9_29, D3DXMatrixRotationYawPitchRoll)       /* (out, yaw, pitch, roll): roll (z), then pitch (x), then yaw (y) */
{
    float y, p, r; uint32_t a = ARG(1), b = ARG(2), d = ARG(3); memcpy(&y, &a, 4); memcpy(&p, &b, 4); memcpy(&r, &d, 4);
    float sy = sinf(y), cy = cosf(y), sp = sinf(p), cp = cosf(p), sr = sinf(r), cr = cosf(r);
    Mat m = {{{cr * cy + sr * sp * sy, sr * cp, cr * -sy + sr * sp * cy, 0},
              {-sr * cy + cr * sp * sy, cr * cp, sr * sy + cr * sp * cy, 0},
              {cp * sy, -sp, cp * cy, 0},
              {0, 0, 0, 1}}};
    stm(ARG(0), &m); RET(ARG(0), 4);
}
IMPL(d3dx9_29, D3DXVec3Normalize)
{
    float v[3]; ldv(ARG(1), v, 3); float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l == 0) v[0] = v[1] = v[2] = 0; else for (int k = 0; k < 3; k++) v[k] /= l;
    stv(ARG(0), v, 3); RET(ARG(0), 2);
}
IMPL(d3dx9_29, D3DXVec3TransformCoord)               /* (out, v, m): w-divided */
{
    float v[3], o[4]; ldv(ARG(1), v, 3); Mat m = ldm(ARG(2));
    for (int j = 0; j < 4; j++) o[j] = v[0] * m.m[0][j] + v[1] * m.m[1][j] + v[2] * m.m[2][j] + m.m[3][j];
    for (int j = 0; j < 3; j++) o[j] = o[3] != 0 ? o[j] / o[3] : 0;
    stv(ARG(0), o, 3); RET(ARG(0), 3);
}
IMPL(d3dx9_29, D3DXVec3TransformNormal)
{
    float v[3], o[3]; ldv(ARG(1), v, 3); Mat m = ldm(ARG(2));
    for (int j = 0; j < 3; j++) o[j] = v[0] * m.m[0][j] + v[1] * m.m[1][j] + v[2] * m.m[2][j];
    stv(ARG(0), o, 3); RET(ARG(0), 3);
}
IMPL(d3dx9_29, D3DXVec4Transform)
{
    float v[4], o[4]; ldv(ARG(1), v, 4); Mat m = ldm(ARG(2));
    for (int j = 0; j < 4; j++) o[j] = v[0] * m.m[0][j] + v[1] * m.m[1][j] + v[2] * m.m[2][j] + v[3] * m.m[3][j];
    stv(ARG(0), o, 4); RET(ARG(0), 3);
}
IMPL(d3dx9_29, D3DXPlaneNormalize)                    /* (out, plane): divided by the length of (a, b, c) */
{
    float p[4]; ldv(ARG(1), p, 4); float l = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (l == 0) p[0] = p[1] = p[2] = p[3] = 0; else for (int k = 0; k < 4; k++) p[k] /= l;
    stv(ARG(0), p, 4); RET(ARG(0), 2);
}
IMPL(d3dx9_29, D3DXPlaneTransform)                    /* (out, plane, m): the plane as a row vector times m (m is the inverse transpose) */
{
    float p[4], o[4]; ldv(ARG(1), p, 4); Mat m = ldm(ARG(2));
    for (int j = 0; j < 4; j++) o[j] = p[0] * m.m[0][j] + p[1] * m.m[1][j] + p[2] * m.m[2][j] + p[3] * m.m[3][j];
    stv(ARG(0), o, 4); RET(ARG(0), 3);
}

/* ---- shaders: the best profiles the device supports (shader model 3) ---- */
static uint32_t profile_str(const char *s)
{
    static uint32_t vs, ps;
    uint32_t *slot = s[0] == 'v' ? &vs : &ps;
    if (!*slot) { *slot = heap_alloc(w32_process_heap, 8, 8); strcpy((char *)GP(*slot), s); }
    return *slot;
}
IMPL(d3dx9_29, D3DXGetVertexShaderProfile) { RET(profile_str("vs_3_0"), 1); }
IMPL(d3dx9_29, D3DXGetPixelShaderProfile) { RET(profile_str("ps_3_0"), 1); }

/* not yet: logged once, E_FAIL */
#define NOT_YET_BODY(name, n) { static int once; if (!once++) fprintf(stderr, "d3dx9: %s is not implemented yet\n", name); RET(0x80004005u, n); }
IMPL(d3dx9_29, D3DXAssembleShader) NOT_YET_BODY("D3DXAssembleShader", 7)
IMPL(d3dx9_29, D3DXAssembleShaderFromFileA) NOT_YET_BODY("D3DXAssembleShaderFromFileA", 6)
IMPL(d3dx9_29, D3DXCompileShaderFromFileA) NOT_YET_BODY("D3DXCompileShaderFromFileA", 9)
IMPL(d3dx9_29, D3DXCreateBuffer) NOT_YET_BODY("D3DXCreateBuffer", 2)
IMPL(d3dx9_29, D3DXGetShaderConstantTable) NOT_YET_BODY("D3DXGetShaderConstantTable", 2)
IMPL(d3dx9_29, D3DXLoadSurfaceFromSurface) NOT_YET_BODY("D3DXLoadSurfaceFromSurface", 8)
IMPL(d3dx9_29, D3DXSaveTextureToFileA) NOT_YET_BODY("D3DXSaveTextureToFileA", 4)
