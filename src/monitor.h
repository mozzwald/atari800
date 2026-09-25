#ifndef MONITOR_H_
#define MONITOR_H_

#include "config.h"
#include <stdio.h>

#include "atari.h"

int MONITOR_Run(void);

#ifdef MONITOR_HINTS
void MONITOR_PreloadLabelFile(char *filename);
#endif

#ifdef MONITOR_TRACE
extern FILE *MONITOR_trace_file;
#define MONITOR_TRACE_CAPACITY 1024
#define MONITOR_TRACE_LINE_SIZE 256
typedef struct {
	unsigned long long seq;
	char line[MONITOR_TRACE_LINE_SIZE];
} MONITOR_trace_entry;
void MONITOR_TraceSetEnabled(int enabled);
void MONITOR_TraceClear(void);
int MONITOR_TraceGetStatus(unsigned long long *next_seq, unsigned long long *dropped);
size_t MONITOR_TraceCount(void);
size_t MONITOR_TraceRead(unsigned long long since_seq, MONITOR_trace_entry *entries, size_t max_entries);
typedef struct {
	unsigned long long seq;
	unsigned short addr;
	unsigned char value;
	unsigned short pc;
	unsigned int frame;
	unsigned int cycle;
} MONITOR_bank_trace_entry;
void MONITOR_BankTraceConfigure(unsigned short start_addr, unsigned short end_addr);
void MONITOR_BankTraceSetEnabled(int enabled);
void MONITOR_BankTraceClear(void);
void MONITOR_BankTraceCapture(unsigned short addr, unsigned char value, unsigned short pc,
				unsigned int frame, unsigned int cycle);
int MONITOR_BankTraceGetStatus(unsigned short *start_addr, unsigned short *end_addr,
				unsigned long long *next_seq, unsigned long long *dropped, size_t *count);
size_t MONITOR_BankTraceRead(unsigned long long since_seq, MONITOR_bank_trace_entry *entries, size_t max_entries);
typedef struct {
	unsigned long long seq;
	unsigned short addr;
	unsigned char value;
	unsigned char is_write;
	unsigned short pc;
	int bank;
	unsigned int frame;
	unsigned int cycle;
} MONITOR_ram_trace_entry;
void MONITOR_RamTraceConfigure(unsigned short start_addr, unsigned short end_addr, int reads, int writes);
void MONITOR_RamTraceSetEnabled(int enabled);
void MONITOR_RamTraceClear(void);
void MONITOR_RamTraceCapture(unsigned short addr, unsigned char value, int is_write, unsigned short pc);
int MONITOR_RamTraceGetStatus(unsigned short *start_addr, unsigned short *end_addr, int *reads, int *writes,
				unsigned long long *next_seq, unsigned long long *dropped, size_t *count);
size_t MONITOR_RamTraceRead(unsigned long long since_seq, MONITOR_ram_trace_entry *entries, size_t max_entries);
void MONITOR_TraceCaptureState(UWORD pc, UBYTE a, UBYTE x, UBYTE y, UBYTE s,
				char n, char v, char z, char c);
#endif

#ifdef MONITOR_BREAK
void MONITOR_BBRK_on(void);
void MONITOR_BPC(char *arg);
extern UWORD MONITOR_break_addr;
extern UBYTE MONITOR_break_step;
extern UBYTE MONITOR_break_ret;
extern UBYTE MONITOR_break_brk;
extern int MONITOR_ret_nesting;
#endif

extern const UBYTE MONITOR_optype6502[256];

void MONITOR_Exit(void);
void MONITOR_ShowState(FILE *fp, UWORD pc, UBYTE a, UBYTE x, UBYTE y, UBYTE s,
                char n, char v, char z, char c);
UWORD MONITOR_ShowInstruction(FILE *fp, UWORD pc);
UWORD MONITOR_Disassemble(FILE *fp, UWORD addr, int count);
UWORD MONITOR_DisassembleLoop(FILE *fp, UWORD addr);
void MONITOR_ShowHistory(FILE *fp);
void MONITOR_ShowLastJumps(FILE *fp);
void MONITOR_ShowStack(FILE *fp, UBYTE sp, int count);
void MONITOR_ShowDisplayList(FILE *fp, UWORD addr, int count);

#ifdef MONITOR_HINTS
void MONITOR_ShowLabels(FILE *fp, int include_builtin, int limit);
#endif

#ifdef MONITOR_BREAKPOINTS

/* Breakpoint conditions */

#define MONITOR_BREAKPOINT_OR          1
#define MONITOR_BREAKPOINT_FLAG_CLEAR  2
#define MONITOR_BREAKPOINT_FLAG_SET    3

/* these three may be ORed together and must be ORed with MONITOR_BREAKPOINT_PC .. MONITOR_BREAKPOINT_WRITE */
#define MONITOR_BREAKPOINT_LESS        1
#define MONITOR_BREAKPOINT_EQUAL       2
#define MONITOR_BREAKPOINT_GREATER     4

#define MONITOR_BREAKPOINT_PC          8
#define MONITOR_BREAKPOINT_A           16
#define MONITOR_BREAKPOINT_X           32
#define MONITOR_BREAKPOINT_Y           40
#define MONITOR_BREAKPOINT_S           48
#define MONITOR_BREAKPOINT_READ        64
#define MONITOR_BREAKPOINT_WRITE       128
#define MONITOR_BREAKPOINT_MEMORY      256
#define MONITOR_BREAKPOINT_ACCESS      (MONITOR_BREAKPOINT_READ | MONITOR_BREAKPOINT_WRITE)

typedef struct {
	UBYTE enabled;
	UWORD condition;
	UWORD value;
	UWORD m_addr; /* only for MEM: */
} MONITOR_breakpoint_cond;

#define MONITOR_BREAKPOINT_TABLE_MAX  20
extern MONITOR_breakpoint_cond MONITOR_breakpoint_table[MONITOR_BREAKPOINT_TABLE_MAX];
extern int MONITOR_breakpoint_table_size;
extern int MONITOR_breakpoints_enabled;

#endif /* MONITOR_BREAKPOINTS */

#ifdef MONITOR_PROFILE
typedef struct {
	unsigned long count; /* number of times executed since last reset */
	unsigned long cycles; /* number of cycles executed since last reset */
} MONITOR_coverage_rec;
extern MONITOR_coverage_rec MONITOR_coverage[0x10000];
extern unsigned long MONITOR_coverage_insns;
extern unsigned long MONITOR_coverage_cycles;

#endif /* MONITOR_PROFILE */

#endif /* MONITOR_H_ */
