static int top              = 1;
static int item_padding     = 20;
static const char *fonts[]  = { "DejaVu Sans Mono:size=10.5" };
static const char *prompt   = NULL;
static uint32_t colors[][2] = {
	/*               fg          bg          */
	[SchemeNorm] = { 0xddddddff, 0x222033ff },
	[SchemeSel]  = { 0x9186dbff, 0x222033ff },
	[SchemeOut]  = { 0x000000ff, 0x00ffffff },
};

/* If nonzero, use vertical list with given number of lines */
static unsigned int lines   = 0;

/*
 * Characters not considered part of a word while deleting words
 * for example: " /?\"&[]"
 */
static const char worddelimiters[] = " ";
