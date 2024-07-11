static int top              = 1;
static int item_padding     = 20;
static const char *fonts[]  = { "DejaVu Sans Mono:size=10.5" };
static const char *prompt   = NULL;
static uint32_t colors[][2] = {
	/*               fg          bg          */
	[SchemeNorm] = { 0xddddddff, 0x222033ff },
	[SchemeSel]  = { 0x9186dbff, 0x222033ff },
};

/* If nonzero, use vertical list with given number of lines */
static unsigned int lines   = 0;
