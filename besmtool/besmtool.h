#define ZBYTES	6144	/* size of zone in bytes */
#define MAXZ	010000	/* max size of disk is about 25 Mbytes */

/*
 * A volume is either a numbered disk image or an АРФА region (область), a
 * single Unix file selected by "arfa:<name>"/a non-numeric argument.
 * open_disk() opens the primary volume, open_from_disk() a copy source.
 */
extern char *besm_arfa_dir;
extern char *besm_arfa_region;
extern char *besm_from_arfa;
void *open_disk (unsigned diskno, unsigned mode);
void *open_from_disk (unsigned diskno, unsigned mode);

void list_all_disks (void);
void list_disk (unsigned diskno);
void passports (unsigned diskno, unsigned start);
void erase_disk (unsigned diskno, unsigned start, unsigned length, int format);
void view_disk (unsigned diskno, unsigned start, unsigned length, char *encoding);
void dump_disk (unsigned diskno, unsigned start, unsigned length);
void check_disk (unsigned diskno, unsigned start, unsigned length);
void search_disk (unsigned diskno, unsigned char *pattern, unsigned start, unsigned length);
void file_to_disk (unsigned to_diskno, unsigned to_start, unsigned length,
	char *from_file, unsigned from_start);
void dir_to_disk (unsigned to_diskno, char *from_dir);
void disk_to_disk (unsigned to_diskno, unsigned to_start, unsigned length,
	unsigned from_diskno, unsigned from_start);
void disk_to_file (unsigned from_diskno, unsigned from_start, unsigned length,
	char *to_file);
