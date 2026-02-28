#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

PSP_MODULE_INFO("Minesweeper", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(20480);

// ── Display ───────────────────────────────────────────────────────────────────
#define SCR_W  480
#define SCR_H  272
#define BUF_W  512
#define VRAM   0x44000000

static unsigned int* const FB0 = (unsigned int*)(VRAM);
static unsigned int* const FB1 = (unsigned int*)(VRAM + BUF_W*SCR_H*4);
static int drawIdx = 0;
#define BUF (drawIdx ? FB1 : FB0)

static inline void dot(int x, int y, unsigned int c) {
    if ((unsigned)x < SCR_W && (unsigned)y < SCR_H)
        BUF[y * BUF_W + x] = c;
}

static unsigned int __attribute__((aligned(64))) guList[16384];

static void guInit(void) {
    sceGuInit();
    sceGuStart(GU_DIRECT, guList);
    sceGuDrawBuffer (GU_PSM_8888,(void*)0,               BUF_W);
    sceGuDispBuffer (SCR_W,SCR_H,(void*)(BUF_W*SCR_H*4), BUF_W);
    sceGuDepthBuffer((void*)(BUF_W*SCR_H*8),              BUF_W);
    sceGuOffset(2048-SCR_W/2, 2048-SCR_H/2);
    sceGuViewport(2048,2048,SCR_W,SCR_H);
    sceGuScissor(0,0,SCR_W,SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuFinish(); sceGuSync(0,0);
    sceGuDisplay(GU_TRUE);
}

static void flip(void) { sceGuSwapBuffers(); drawIdx^=1; }

// ── Colors (ABGR) ─────────────────────────────────────────────────────────────
// iOS 17 exact system colors
#define C_BG        0xFFF2F2F7   // systemBackground
#define C_BG2       0xFFE5E5EA   // secondarySystemBackground  
#define C_CARD      0xFFFFFFFF   // white card
#define C_SEP       0xFFD1D1D6   // separator
#define C_LABEL     0xFF1C1C1E   // label
#define C_GRAY2     0xFF636366   // secondary label
#define C_GRAY3     0xFF8E8E93   // tertiary label
#define C_FILL      0xFFE5E5EA   // system fill
#define C_FILL2     0xFFCECED6   // secondary fill

// iOS system accent colors
#define C_BLUE      0xFFFF6B00   // #006BFF
#define C_GREEN     0xFF34C759   // #59C734
#define C_RED       0xFF3B30FF   // #FF303B
#define C_ORANGE    0xFF0094FF   // #FF9400
#define C_INDIGO    0xFFD65658   // #5856D6
#define C_TEAL      0xFFE6AD32   // #32ADE6
#define C_PURPLE    0xFFAF52DE
#define C_PINK      0xFF7090FF   // #FF9070

// Tile specific
#define C_T_FACE    0xFFDCDCE4
#define C_T_SHINE   0xFFECECF4
#define C_T_SHADOW  0xFF9898A2
#define C_T_OPEN    0xFFF2F2F7
#define C_T_BORDER  0xFFC8C8D0

static const unsigned int NCLR[9] = {
    0, C_BLUE, C_GREEN, C_RED, C_INDIGO, C_ORANGE, C_TEAL, C_LABEL, C_GRAY3
};

// ── Layout — carefully calculated to fit SCR_H=272 ───────────────────────────
// Top bar: y=4, height=44  → bottom at y=48
// Grid starts: y=52
// Tile size: 22px, gap: 2px, step: 24px
// Grid height: 9*24-2 = 214px
// Grid bottom: 52+214 = 266 → fits in 272 ✓

#define GN      9
#define BOMBS   15
#define TSAFE   2

#define TILE    22
#define GAP     2
#define STEP    (TILE+GAP)
#define GPIX    (GN*STEP-GAP)       // 9*24-2 = 214
#define GRID_X  ((SCR_W-GPIX)/2)    // (480-214)/2 = 133
#define GRID_Y  52                   // starts at 52, ends at 52+214=266 ✓

// ── Primitives ────────────────────────────────────────────────────────────────
static unsigned int lerpC(unsigned int a, unsigned int b, int t) {
    int r =(a&0xFF)      +(((int)(b&0xFF)      -(a&0xFF))      *t>>8);
    int g =((a>>8)&0xFF) +(((int)((b>>8)&0xFF) -((a>>8)&0xFF)) *t>>8);
    int bl=((a>>16)&0xFF)+(((int)((b>>16)&0xFF)-((a>>16)&0xFF))*t>>8);
    return 0xFF000000|((bl&0xFF)<<16)|((g&0xFF)<<8)|(r&0xFF);
}

static void hline(int x,int y,int w,unsigned int c){
    for(int i=x;i<x+w;i++) dot(i,y,c);
}
static void vline(int x,int y,int h,unsigned int c){
    for(int j=y;j<y+h;j++) dot(x,j,c);
}
static void fillRect(int x,int y,int w,int h,unsigned int c){
    for(int j=y;j<y+h;j++) hline(x,j,w,c);
}

static void rrFill(int x,int y,int w,int h,int r,unsigned int c){
    if(r<1){fillRect(x,y,w,h,c);return;}
    fillRect(x+r,y,w-2*r,h,c);
    fillRect(x,y+r,r,h-2*r,c);
    fillRect(x+w-r,y+r,r,h-2*r,c);
    for(int cy=0;cy<r;cy++) for(int cx=0;cx<r;cx++){
        int dx=r-1-cx,dy=r-1-cy;
        if(dx*dx+dy*dy<=r*r){
            dot(x+cx,       y+cy,      c);
            dot(x+w-1-cx,   y+cy,      c);
            dot(x+cx,       y+h-1-cy,  c);
            dot(x+w-1-cx,   y+h-1-cy,  c);
        }
    }
}

static void rrStroke(int x,int y,int w,int h,int r,unsigned int c){
    hline(x+r,y,w-2*r,c); hline(x+r,y+h-1,w-2*r,c);
    vline(x,y+r,h-2*r,c); vline(x+w-1,y+r,h-2*r,c);
    for(int cy=0;cy<r;cy++) for(int cx=0;cx<r;cx++){
        int dx=r-1-cx,dy=r-1-cy,d2=dx*dx+dy*dy;
        if(d2<=r*r && d2>=(r-1)*(r-1)){
            dot(x+cx,       y+cy,      c);
            dot(x+w-1-cx,   y+cy,      c);
            dot(x+cx,       y+h-1-cy,  c);
            dot(x+w-1-cx,   y+h-1-cy,  c);
        }
    }
}

static void circle(int cx,int cy,int r,unsigned int c){
    for(int dy=-r;dy<=r;dy++) for(int dx=-r;dx<=r;dx++)
        if(dx*dx+dy*dy<=r*r) dot(cx+dx,cy+dy,c);
}

// ── Clean pixel font — designed wide & readable ───────────────────────────────
// 7x9 grid (wider than 5x7), looks much more modern at scale 1
static const unsigned char PF[][7] = {
    // Each byte = column, bits 0-8 = rows top to bottom
    // Space
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 0
    {0x7C,0xFE,0x82,0x82,0x82,0xFE,0x7C},
    // 1
    {0x00,0x84,0xFE,0xFE,0x80,0x00,0x00},
    // 2
    {0xC4,0xE2,0xA2,0xA2,0x9E,0x9C,0x00},
    // 3
    {0x42,0x82,0x9A,0x9A,0xFE,0x66,0x00},
    // 4
    {0x30,0x28,0x24,0xFE,0xFE,0x20,0x00},
    // 5
    {0x5E,0x9E,0x9A,0x9A,0xF2,0x62,0x00},
    // 6
    {0x7C,0xFE,0x9A,0x9A,0xF2,0x62,0x00},
    // 7
    {0x02,0x02,0xE2,0xF2,0x1E,0x06,0x00},
    // 8
    {0x6C,0xFE,0x9A,0x9A,0xFE,0x6C,0x00},
    // 9
    {0x4C,0x9E,0x9A,0x9A,0xFE,0x7C,0x00},
    // A
    {0xF8,0xFC,0x16,0x16,0xFC,0xF8,0x00},
    // B
    {0xFE,0xFE,0x92,0x92,0xFE,0x6C,0x00},
    // C
    {0x7C,0xFE,0x82,0x82,0xC6,0x44,0x00},
    // D
    {0xFE,0xFE,0x82,0x82,0xFE,0x7C,0x00},
    // E
    {0xFE,0xFE,0x92,0x92,0x82,0x82,0x00},
    // F
    {0xFE,0xFE,0x12,0x12,0x02,0x02,0x00},
    // G
    {0x7C,0xFE,0x82,0x92,0xF2,0x70,0x00},
    // H
    {0xFE,0xFE,0x10,0x10,0xFE,0xFE,0x00},
    // I
    {0x00,0x82,0xFE,0xFE,0x82,0x00,0x00},
    // J (unused but kept for index alignment)
    {0x40,0xC2,0x82,0xFE,0x7E,0x00,0x00},
    // K
    {0xFE,0xFE,0x10,0x28,0xC6,0x82,0x00},
    // L
    {0xFE,0xFE,0x80,0x80,0x80,0x80,0x00},
    // M
    {0xFE,0x0C,0x30,0x30,0x0C,0xFE,0xFE},
    // N
    {0xFE,0x0C,0x10,0x20,0xC0,0xFE,0x00},
    // O
    {0x7C,0xFE,0x82,0x82,0xFE,0x7C,0x00},
    // P
    {0xFE,0xFE,0x12,0x12,0x1E,0x0C,0x00},
    // Q (unused)
    {0x7C,0xFE,0x82,0xA2,0xFE,0xFC,0x00},
    // R
    {0xFE,0xFE,0x12,0x32,0xFE,0xCC,0x00},
    // S
    {0x4C,0x9E,0x92,0x92,0xF2,0x62,0x00},
    // T
    {0x02,0x02,0xFE,0xFE,0x02,0x02,0x00},
    // U
    {0x7E,0xFE,0x80,0x80,0xFE,0x7E,0x00},
    // V
    {0x1E,0x7E,0xC0,0xC0,0x7E,0x1E,0x00},
    // W
    {0x3E,0xFE,0xC0,0x70,0xC0,0xFE,0x3E},
    // X
    {0xC6,0xEE,0x38,0x38,0xEE,0xC6,0x00},
    // Y
    {0x06,0x1E,0xF8,0xF8,0x1E,0x06,0x00},
    // Z
    {0xC2,0xE2,0xB2,0x9A,0x8E,0x86,0x00},
};

static int charIdx(char c){
    if(c>='a'&&c<='z') c-=32;
    if(c>='0'&&c<='9') return c-'0'+1;   // 1-10
    if(c>='A'&&c<='Z') return c-'A'+11;  // 11-36
    return 0; // space
}

// Draw char at (x,y), scale s (pixels per bit)
static void drawChar(int x, int y, char c, unsigned int col, int s){
    int idx = charIdx(c);
    const unsigned char* b = PF[idx];
    for(int col2=0; col2<7; col2++){
        for(int row=0; row<9; row++){
            if(b[col2] & (1<<row)){
                for(int sy=0;sy<s;sy++)
                    for(int sx=0;sx<s;sx++)
                        dot(x+col2*s+sx, y+row*s+sy, col);
            }
        }
    }
}

static int charW(int s){ return 7*s+s; } // 7 cols + 1 gap

static void drawText(int x, int y, const char* s, unsigned int col, int sc){
    while(*s){
        if(*s==' '){ x+=charW(sc); s++; continue; }
        drawChar(x,y,*s,col,sc);
        x+=charW(sc);
        s++;
    }
}

static int textWidth(const char* s, int sc){
    int w=0;
    while(*s){ w+=charW(sc); s++; }
    return w>0 ? w-sc : 0;
}

static void drawTextC(int cx, int y, const char* s, unsigned int col, int sc){
    drawText(cx - textWidth(s,sc)/2, y, s, col, sc);
}

// ── Game state ────────────────────────────────────────────────────────────────
typedef struct { int mine,rev,flag,adj; } Cell;
Cell  G[GN][GN];
int   curX,curY,dead,won,first,nflag,nrev,hitx,hity,tsec,tframe;

void newGame(void){
    memset(G,0,sizeof G);
    curX=curY=GN/2;
    dead=won=nflag=nrev=0;
    first=1; hitx=hity=-1; tsec=tframe=0;
}

static void place(int sx, int sy){
    srand((unsigned)time(NULL));
    int p=0;
    while(p<BOMBS){
        int x=rand()%GN, y=rand()%GN;
        if(abs(x-sx)<=TSAFE && abs(y-sy)<=TSAFE) continue;
        if(!G[x][y].mine){ G[x][y].mine=1; p++; }
    }
    for(int x=0;x<GN;x++) for(int y=0;y<GN;y++){
        if(G[x][y].mine) continue;
        int c=0;
        for(int dx=-1;dx<=1;dx++) for(int dy=-1;dy<=1;dy++){
            int nx=x+dx,ny=y+dy;
            if(nx>=0&&nx<GN&&ny>=0&&ny<GN&&G[nx][ny].mine) c++;
        }
        G[x][y].adj=c;
    }
}

static void flood(int x, int y){
    if(x<0||x>=GN||y<0||y>=GN) return;
    if(G[x][y].rev||G[x][y].flag) return;
    G[x][y].rev=1; nrev++;
    if(!G[x][y].adj && !G[x][y].mine)
        for(int dx=-1;dx<=1;dx++) for(int dy=-1;dy<=1;dy++) flood(x+dx,y+dy);
}

static void doReveal(int x, int y){
    if(first){
        first=0; place(x,y);
        flood(x,y);
        // ensure opening — if only 1 cell revealed, force open neighbors too
        if(nrev<=1)
            for(int dx=-1;dx<=1;dx++) for(int dy=-1;dy<=1;dy++) flood(x+dx,y+dy);
        return;
    }
    if(G[x][y].flag) return;
    if(G[x][y].mine){
        dead=1; hitx=x; hity=y;
        for(int i=0;i<GN;i++) for(int j=0;j<GN;j++)
            if(G[i][j].mine) G[i][j].rev=1;
        return;
    }
    flood(x,y);
    if(nrev==GN*GN-BOMBS) won=1;
}

static void doFlag(int x, int y){
    if(G[x][y].rev) return;
    G[x][y].flag ^= 1;
    nflag += G[x][y].flag ? 1 : -1;
}

// ── Rendering ─────────────────────────────────────────────────────────────────
static void drawBg(void){
    // Solid iOS system background — no gradient, cleaner
    for(int y=0;y<SCR_H;y++)
        for(int x=0;x<SCR_W;x++)
            BUF[y*BUF_W+x] = C_BG;
}

static void drawTopBar(void){
    // Bar: y=4 to y=48 (height=44)
    int bx=8, by=4, bw=SCR_W-16, bh=44;

    // 2-level shadow
    rrFill(bx+1, by+3, bw, bh, 12, 0x12000000);
    rrFill(bx,   by+1, bw, bh, 12, 0x08000000);
    // White card
    rrFill(bx, by, bw, bh, 12, C_CARD);
    // 1px border
    rrStroke(bx, by, bw, bh, 12, C_SEP);
    // Subtle top shine
    hline(bx+12, by+1, bw-24, 0x18FFFFFF);

    char buf[16];

    // ── LEFT: bomb count badge ──
    // Red pill: x=18, y=10, w=46, h=24
    rrFill(18, 10, 46, 26, 13, C_RED);
    hline(24, 11, 34, 0x22FFFFFF); // top shine on badge
    sprintf(buf, "%d", BOMBS-nflag);
    drawTextC(41, 14, buf, C_CARD, 1);

    // ── CENTER: title ──
    drawTextC(SCR_W/2, 9, "MINESWEEPER", C_LABEL, 1);
    // ── CENTER: controls hint ──
    drawTextC(SCR_W/2, 30, "X=REVEAL  O=FLAG  START=NEW", C_GRAY3, 1);

    // ── RIGHT: timer badge ──
    rrFill(SCR_W-64, 10, 46, 26, 13, C_LABEL);
    hline(SCR_W-58, 11, 34, 0x20FFFFFF);
    sprintf(buf, "%d", tsec);
    drawTextC(SCR_W-41, 14, buf, C_CARD, 1);
}

static void drawTile(int gx, int gy){
    int px = GRID_X + gx*STEP;
    int py = GRID_Y + gy*STEP;
    Cell* c = &G[gx][gy];
    int cur = (gx==curX && gy==curY && !dead && !won);

    if(c->rev){
        // Revealed: flat inset feel
        unsigned int bg = (gx==hitx && gy==hity) ? 0xFFFFE5E5 : C_T_OPEN;
        rrFill(px, py, TILE, TILE, 5, bg);
        rrStroke(px, py, TILE, TILE, 5, C_FILL2);

        if(c->mine){
            unsigned int mc = (gx==hitx && gy==hity) ? C_RED : C_LABEL;
            // Circle body
            circle(px+TILE/2, py+TILE/2, 4, mc);
            // 8 spikes
            int spkx[8]={0, 5, 7, 5, 0,-5,-7,-5};
            int spky[8]={-7,-5, 0, 5, 7, 5, 0,-5};
            for(int i=0;i<8;i++){
                dot(px+TILE/2+spkx[i],   py+TILE/2+spky[i],   mc);
                dot(px+TILE/2+spkx[i]/2, py+TILE/2+spky[i]/2, mc);
            }
            // Shine
            dot(px+TILE/2-2, py+TILE/2-2, 0x80FFFFFF);
            dot(px+TILE/2-1, py+TILE/2-2, 0x80FFFFFF);
        } else if(c->adj > 0){
            // Number — centered, scale 1 for TILE=22
            char n[2] = {'0'+c->adj, 0};
            drawTextC(px+TILE/2-3, py+TILE/2-4, n, NCLR[c->adj], 1);
        }

    } else {
        // Unrevealed: soft 3D look
        // Drop shadow (1px offset)
        rrFill(px+1, py+2, TILE, TILE, 5, C_T_SHADOW);
        // Main face
        rrFill(px, py, TILE, TILE, 5, C_T_FACE);
        // Top-left shine band
        rrFill(px+2, py+1, TILE-4, 6, 3, C_T_SHINE);
        vline(px+1, py+5, TILE-9, C_T_SHINE);
        rrStroke(px, py, TILE, TILE, 5, C_T_BORDER);

        if(c->flag){
            // Pole — 2px wide, centered
            int fx = px + TILE/2 - 1;
            int fy = py + 3;
            vline(fx,   fy, 14, C_RED);
            vline(fx+1, fy, 14, C_RED);
            // Flag banner — solid rectangle left of pole, 3 rows
            fillRect(fx-6, fy,   6, 3, C_RED);
            fillRect(fx-6, fy+3, 6, 3, C_RED);
            fillRect(fx-6, fy+6, 6, 3, C_RED);
            // Small gap in middle to make it look like waving stripes
            hline(fx-5, fy+2, 4, C_T_FACE);
            hline(fx-5, fy+5, 4, C_T_FACE);
            // Base
            hline(fx-3, fy+14, 8, C_RED);
            hline(fx-2, fy+15, 6, C_RED);
        }
    }

    // Cursor: orange ring
    if(cur){
        rrStroke(px-2, py-2, TILE+4, TILE+4, 7, C_ORANGE);
        // Outer glow
        rrStroke(px-3, py-3, TILE+6, TILE+6, 8, 0x40FF9400);
    }
}

static void drawGrid(void){
    for(int x=0;x<GN;x++)
        for(int y=0;y<GN;y++)
            drawTile(x,y);
}

static void drawModal(void){
    // Tint whole screen
    unsigned int tint = won ? C_GREEN : C_RED;
    for(int y=0;y<SCR_H;y++)
        for(int x=0;x<SCR_W;x++)
            BUF[y*BUF_W+x] = lerpC(BUF[y*BUF_W+x], tint, 42);

    // Modal card — centered
    int cw=280, ch=112, ox=(SCR_W-cw)/2, oy=(SCR_H-ch)/2;

    // Shadow
    rrFill(ox+2, oy+4, cw, ch, 16, 0x28000000);
    rrFill(ox+1, oy+2, cw, ch, 16, 0x14000000);
    // Card
    rrFill(ox, oy, cw, ch, 16, C_CARD);
    rrStroke(ox, oy, cw, ch, 16, C_SEP);
    hline(ox+16, oy+1, cw-32, 0x18FFFFFF);

    // Title
    char buf[32];
    unsigned int tc = won ? C_GREEN : C_RED;
    if(won){
        drawTextC(SCR_W/2, oy+10, "YOU WIN!", tc, 2);
        drawTextC(SCR_W/2, oy+40, "ALL MINES CLEARED", C_GRAY3, 1);
    } else {
        drawTextC(SCR_W/2, oy+10, "GAME OVER", tc, 2);
        drawTextC(SCR_W/2, oy+40, "YOU HIT A MINE", C_GRAY3, 1);
    }
    sprintf(buf, "TIME  %d SEC", tsec);
    drawTextC(SCR_W/2, oy+56, buf, C_GRAY3, 1);

    // Button
    int bx=ox+40, by2=oy+74, bw2=cw-80, bh2=26;
    rrFill(bx, by2, bw2, bh2, 13, tc);
    hline(bx+13, by2+1, bw2-26, 0x22FFFFFF);
    drawTextC(SCR_W/2, by2+8, "PRESS X TO PLAY AGAIN", C_CARD, 1);
}

static void drawFrame(void){
    drawBg();
    drawTopBar();
    drawGrid();
    if(dead || won) drawModal();
}

// ── Callbacks ─────────────────────────────────────────────────────────────────
static int running = 1;
static int exitCb(int a, int b, void* c){ running=0; return 0; }
static int cbThread(SceSize a, void* b){
    int id = sceKernelCreateCallback("Exit", exitCb, NULL);
    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}
static void setupCb(void){
    int id = sceKernelCreateThread("cbt", cbThread, 0x11, 0xFA0, 0, 0);
    if(id>=0) sceKernelStartThread(id, 0, 0);
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main(void){
    setupCb();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    guInit();
    newGame();

    SceCtrlData pad, prev;
    memset(&prev, 0, sizeof prev);

    while(running){
        sceCtrlReadBufferPositive(&pad, 1);
        unsigned int pressed = pad.Buttons & ~prev.Buttons;

        if(!first && !dead && !won){
            if(++tframe >= 60){ tframe=0; tsec++; }
        }

        if(!dead && !won){
            if(pressed & PSP_CTRL_UP)    curY = (curY-1+GN)%GN;
            if(pressed & PSP_CTRL_DOWN)  curY = (curY+1)%GN;
            if(pressed & PSP_CTRL_LEFT)  curX = (curX-1+GN)%GN;
            if(pressed & PSP_CTRL_RIGHT) curX = (curX+1)%GN;
            if(pressed & PSP_CTRL_CROSS)  doReveal(curX, curY);
            if(pressed & PSP_CTRL_CIRCLE) doFlag(curX, curY);
        } else {
            if(pressed & PSP_CTRL_CROSS) newGame();
        }
        if(pressed & PSP_CTRL_START) newGame();

        drawFrame();
        sceDisplayWaitVblankStart();
        flip();
        prev = pad;
    }

    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
