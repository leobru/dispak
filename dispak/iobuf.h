#define TKH000          01442505500000000ull
#define UNDERBANG3      02645553226655133ull    /* _!_!_! */
#define EKONEC          01122505613222466ull    /* ЕКОНЕЦ */

#define MAXVOL  12
#define U(c)    ((unsigned) c & 0xff)

#define VOL_READ_ONLY   0
#define VOL_READ_WRITE  1
#define VOL_CHUNK       2

/* ibword tags  */
#define W_IADDR         1
#define W_DATA          2
#define W_CODE          4

struct ibword   {
	uchar   tag;
	uchar   spare;
	word_t  w;
};

struct passport {
	alureg_t                user;
	uint                    entry;
	uint			intercept;
	uchar                   tele, keep, spec;
	ushort                  nvol;
	ushort                  lprlim;
	uint                    phys;
	struct vol      {
		uchar                   wr;
		uchar                   u;
		uint                    volno;
		ushort			offset;
	}                       vol[MAXVOL];
	uint                    arr_end;        /* offset to input array 1 end */
	alureg_t                glavn;          /* ГЛА: шифр главной задачи,
						   0 - no ГЛА */
};
