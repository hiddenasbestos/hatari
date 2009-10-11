/*
  Hatari - calculate.c

  Copyright (C) 1994, 2009 by Eero Tamminen

  This file is distributed under the GNU Public License, version 2 or at
  your option any later version. Read the file gpl.txt for details.

  calculate.c - parse numbers, number ranges and expressions. Supports
  most unary and binary operations, parenthesis and order of precedence.
  Originally based on code from my Clac calculator MiNT filter version.
*/
const char Eval_fileid[] = "Hatari calculate.c : " __DATE__ " " __TIME__;

#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <SDL_types.h>
#include "calculate.h"
#include "configuration.h"

/* define which character indicates which type of number on expression  */
#define PREFIX_BIN '%'                            /* binary decimal       */
#define PREFIX_DEC '#'                             /* normal decimal       */
#define PREFIX_HEX '$'                             /* hexadecimal          */

/* define error codes                                                   */
#define CLAC_EXP_ERR "No expression given"
#define CLAC_GEN_ERR "Syntax error"
#define CLAC_PAR_ERR "Mismatched parenthesis"
#define CLAC_DEF_ERR "Undefined result (1/0)"
#define CLAC_STK_ERR "Operation/value stack full"
#define CLAC_OVF_ERR "Overflow"
#define CLAC_OVR_ERR "Mode overflow"
#define CLAC_PRG_ERR "Internal program error"

/* define internal allocation sizes (should be enough ;-)		*/
#define PARDEPTH_MAX	64		/* max. parenth. nesting depth	*/
#define OSTACK_MAX	128		/* size of the operator stack	*/
#define VSTACK_MAX	128		/* size of the value stack	*/

/* operation with lowest precedence, used to finish calculations */
#define LOWEST_PREDECENCE '|'

/* globals + function identifier stack(s)				*/
static struct {
	const char *error;		/* global error code		*/
	bool valid;			/* value validation		*/
} id = {0, 0};

/* parenthesis and function stacks					*/
static struct {
	int idx;			/* parenthesis level		*/
	int max;			/* maximum idx			*/
	int opx[PARDEPTH_MAX + 1];	/* current op index for par	*/
	int vax[PARDEPTH_MAX + 1];	/* current val index for par	*/
} par = {0, PARDEPTH_MAX, {0}, {0}};

static struct {					/* operator stack	*/
	int idx;
	int max;
	char buf[OSTACK_MAX + 1];
} op = {0, OSTACK_MAX, ""};

static struct value_stk {			/* value stack	*/
	int idx;
	int max;
	long long buf[VSTACK_MAX + 1];
} val = {0, VSTACK_MAX, {0}};

/* -------------------------------------------------------------------- */
/* macros								*/

/* increment stack index and put value on stack (ie. PUSH)		*/
#define PUSH(stk,val) \
	if((stk).idx < (stk).max) {		\
		(stk).idx += 1;			\
		(stk).buf[(stk).idx] = (val);	\
	} else {				\
		id.error = CLAC_STK_ERR;	\
	}

/* -------------------------------------------------------------------- */
/* declare subfunctions							*/

/* parse in-between operations	*/
static void operation(long long value, char op);
/* parse unary operators	*/
static void unary (char op);
/* apply a prefix to a value */
static void apply_prefix(void);
/* juggle stacks, if possible	*/
static void eval_stack(void);
/* operator -> operator level	*/
static int get_level(int stk_offset);
/* evaluate operation		*/
static long long apply_op(char op, long long x, long long y);

/* increase parenthesis level	*/
static void open_bracket(void);
/* decrease parenthesis level	*/
static long long close_bracket(long long x);


/**
 * Parse & set an (unsigned) number, assuming it's in the configured
 * default number base unless it has a prefix:
 * - '$' / '0x' / '0h' => hexadecimal
 * - '#' / '0d' => normal decimal
 * - '%' / '0b' => binary decimal
 * - '0o' => octal decimal
 * Return how many characters were parsed or zero for error.
 */
static int getNumber(const char *str, long long *number, int *nbase)
{
	char *end;
	const char const *start = str;
	int base = ConfigureParams.Log.nNumberBase;
	long long value;

	/* determine correct number base */
	if (str[0] == '0') {

		/* 0x & 0h = hex, 0d = dec, 0o = oct, 0b = bin ? */
		switch(str[1]) {
		case 'b':
			base = 2;
			break;
		case 'o':
			base = 8;
			break;
		case 'd':
			base = 10;
			break;
		case 'h':
		case 'x':
			base = 16;
			break;
		default:
			str -= 2;
		}
		str += 2;
	}
	else if (!isxdigit(str[0])) {

		/* doesn't start with (hex) number -> is it prefix? */
		switch (*str++) {
		case PREFIX_BIN:
			base = 2;
			break;
		case PREFIX_DEC:
			base = 10;
			break;
		case PREFIX_HEX:
			base = 16;
			break;
		default:
			fprintf(stderr, "Unrecognized number prefix in '%s'!\n", start);
			return 0;
		}
	}
	*nbase = base;

	/* parse number */
	errno = 0;
	value = strtoll(str, &end, base);
	if (errno == ERANGE && (value == LLONG_MAX || value == LLONG_MIN)) {
		fprintf(stderr, "Under/overflow with value '%s'!\n", start);
		return 0;
	}
	if ((errno != 0 && value == 0) || end == str) {
		fprintf(stderr, "Value '%s' is empty!\n", start);
		return 0;
	}
	*number = value;
	return end - start;
}


/**
 * Parse & set an (unsigned) number, assume it's in the configured
 * default number base unless it has a suitable prefix.
 * Return true for success and false for failure.
 */
bool Eval_Number(const char *str, Uint32 *number)
{
	int offset, base = 0;
	long long value = 0;
	
	offset = getNumber(str, &value, &base);
	if (!offset) {
		return false;
	}
	if (str[offset]) {
		const char *basestr;

		switch (base) {
		case 2:
			basestr = "binary";
			break;
		case 8:
			basestr = "octal";
			break;
		case 10:
			basestr = "decimal";
			break;
		case 16:
			basestr = "hexadecimal";
			break;
		default:
			basestr = "unknown";
	}
		fprintf(stderr, "Extra characters in %s based number '%s'!\n",
			basestr, str);
		return false;
	}
	if (value < 0 || value > LONG_MAX) {
		fprintf(stderr, "Number '%s' doesn't fit into Uint32!\n", str);
		return false;
	}
	*number = value;
	return true;
}


/**
 * Get a an adress range, eg. "$fa0000-$fa0100"
 * returns:
 *  0 if OK,
 * -1 if not syntaxically a range,
 * -2 if values are invalid,
 * -3 if syntaxically range, but not value-wise.
 */
static int getRange(char *str1, Uint32 *lower, Uint32 *upper)
{
	bool fDash = false;
	char *str2 = str1;
	int ret = 0;

	while (*str2) {
		if (*str2 == '-') {
			*str2++ = '\0';
			fDash = true;
			break;
		}
		str2++;
	}
	if (!fDash)
		return -1;

	if (!Eval_Number(str1, lower))
		ret = -2;
	else if (!Eval_Number(str2, upper))
		ret = -2;
	else if (*lower > *upper)
		ret = -3;
	*--str2 = '-';
	return ret;
}


/**
 * Parse an adress range, eg. "$fa0000[-$fa0100]" + show appropriate warnings
 * returns:
 * -1 if invalid address or range,
 *  0 if single address,
 * +1 if a range.
 */
int Eval_Range(char *str, Uint32 *lower, Uint32 *upper)
{
	switch (getRange(str, lower, upper)) {
	case 0:
		return 1;
	case -1:
		/* single address, not a range */
		if (!Eval_Number(str, lower))
			return -1;
		return 0;
	case -2:
		fprintf(stderr,"Invalid address values in '%s'!\n", str);
		return -1;
	case -3:
		fprintf(stderr,"Invalid range ($%x > $%x)!\n", *lower, *upper);
		return -1;
	}
	fprintf(stderr, "INTERNAL ERROR: Unknown getRange() return value.\n");
	return -1;
}


/**
 * Evaluate expression.
 * Set given value and parsing offset, return error string or NULL for success.
 */
const char* Eval_Expression(const char *in, long long *out, int *erroff)
{
	/* in	 : expression to evaluate				*/
	/* out	 : final parsed value					*/
	/* value : current parsed value					*/
	/* mark	 : current character in expression			*/
	/* valid : expression validation flag, set when number parsed	*/
	/* end	 : 'expression end' flag				*/
	/* offset: character offset in expression			*/

	int dummy, consumed, offset = 0;
	long long value;
	char mark;
	
	/* Uses global variables:	*/

	par.idx = 0;			/* parenthesis stack pointer	*/
	par.opx[0] = par.vax[0] = 0;	/* additional stack pointers	*/
	op.idx = val.idx = -1;

	id.error = NULL;
	id.valid = false;		/* value validation		*/
	value = 0;

	/* parsing loop, repeated until expression ends */
	do {
		mark = in[offset];
		switch(mark) {
		case '\0':
			break;
		case ' ':
		case '\t':
			offset ++;		/* jump over white space */
			break;
		case '~':			/* prefixes */
			unary(mark);
			offset ++;
			break;
		case '>':			/* operators  */
		case '<':
			offset ++;
			/* check that it's '>>' or '<<' */
			if (in[offset] != mark)
			{
				id.error = CLAC_GEN_ERR;
				break;
			}
			operation (value, mark);
			offset ++;
			break;
		case '|':
		case '&':
		case '^':
		case '+':
		case '-':
		case '*':
		case '/':
			operation (value, mark);
			offset ++;
			break;
		case '(':
			open_bracket ();
			offset ++;
			break;
		case ')':
			value = close_bracket (value);
			offset ++;
			break;
		default:
			/* number needed? */
			if (id.valid == false) {
				consumed = getNumber(&(in[offset]), &value, &dummy);
				/* number parsed? */
				if (consumed) {
					offset += consumed;
					id.valid = true;
					break;
				}
			}
			id.error = CLAC_GEN_ERR;
		}

	/* until exit or error message					*/
	} while(mark && !id.error);

        /* result of evaluation 					*/
        if (val.idx >= 0)
		*out = val.buf[val.idx];

	/* something to return?						*/
	if (!id.error) {
		if (id.valid) {

			/* evaluate rest of the expression		*/
			operation (value, LOWEST_PREDECENCE);
			if (par.idx)			/* mismatched	*/
				id.error = CLAC_PAR_ERR;
			else				/* result out	*/
				*out = val.buf[val.idx];

		} else {
			if ((val.idx < 0) && (op.idx < 0)) {
				id.error = CLAC_EXP_ERR;
				*out = 0;
			} else			/* trailing operators	*/
				id.error = CLAC_GEN_ERR;
		}
	}

	*erroff = offset;
	if (id.error) {
		return id.error;
	}
	return NULL;
}


/* ==================================================================== */
/*			expression evaluation				*/
/* ==================================================================== */

static void operation (long long value, char oper)
{
	/* uses globals par[], id.error[], op[], val[]
	 * operation executed if the next one is on same or lower level
	 */
	/* something to calc? */
	if(id.valid == true) {
		
		/* add new items to stack */
		PUSH(op, oper);
		PUSH(val, value);
		
		/* more than 1 operator  */
		if(op.idx > par.opx[par.idx]) {

			/* but only one value */
			if(val.idx == par.vax[par.idx]) {
				apply_prefix();
			} else {
				/* evaluate all possible operations */
				eval_stack();
			}
		}
		/* next a number needed */
		id.valid = false;
	} else {
		/* pre- or post-operators instead of in-betweens */
		unary(oper);
	}
}

/**
 * handle unary operators
 */
static void unary (char oper)
{
	/* check pre-value operators
	 * have to be parenthesised
	 */
	if(id.valid == false && op.idx < par.opx[par.idx])
	{
		switch(oper) {
		case '+':		/* not needed */
			break;
		case '-':
		case '~':
			PUSH(op, oper);
			break;
		default:
			id.error = CLAC_PRG_ERR;
		}
	}
	else
		id.error = CLAC_GEN_ERR;
}

/**
 * apply a prefix to the current value
 */
static void apply_prefix(void)
{
	long long value = val.buf[val.idx];

	op.idx--;
	switch(op.buf[op.idx]) {
	case '-':
		value = (-value);
		break;
	case '~':
		value = (~value);
		break;
	default:
		id.error = CLAC_PRG_ERR;
	}
	val.buf[val.idx] = value;
	op.buf[op.idx] = op.buf[op.idx + 1];
}

/* -------------------------------------------------------------------- */
/**
 * evaluate operators if precedence allows it
 */
/* evaluate all possible (according to order of precedence) operators	*/
static void eval_stack (void)
{
	/* uses globals par[], op[], val[]	*/

	/* # of operators >= 2 and prev. op-level >= current op-level ?	*/
	while ((op.idx > par.opx[par.idx]) && get_level (-1) >= get_level (0)) {

		/* shorten value stacks by one	*/
		/* + calculate resulting value	*/
		op.idx -= 1;
		val.idx -= 1;
		val.buf[val.idx] = apply_op(op.buf[op.idx],
			val.buf[val.idx], val.buf[val.idx + 1]);

		/* pull the just used operator out of the stack		*/
		op.buf[op.idx] = op.buf[op.idx + 1];
	}
}

/* -------------------------------------------------------------------- */
/**
 * return the precedence level of a given operator
 */
static int get_level (int offset)
{
	/* used globals par[], op[]
	 * returns operator level of: operator[stack idx + offset]
	 */
	switch(op.buf[op.idx + offset]) {
	case '|':      /* binary operations  */
	case '&':
	case '^':
		return 0;
		
	case '>':      /* bit shifting    */
	case '<':
		return 1;
		
	case '+':
	case '-':
		return 2;
		
	case '*':
	case '/':
		return 3;
		
	default:
		id.error = CLAC_PRG_ERR;
	}
	return 6;
}

/* -------------------------------------------------------------------- */
/**
 * apply operator to given values, return the result
 */
static long long apply_op (char opcode, long long value1, long long value2)
{
	/* uses global id.error[]		*/
	/* returns the result of operation	*/

	switch (opcode) {
        case '|':
		value1 |= value2;
		break;
        case '&':
		value1 &= value2;
		break;
        case '^':
		value1 ^= value2;
		break;
        case '>':
		value1 >>= value2;
        case '<':
		value1 <<= value2;
		break;
	case '+':
		value1 += value2;
		break;
	case '-':
		value1 -= value2;
		break;
	case '*':
		value1 *= value2;
		break;
	case '/':
		/* don't divide by zero */
		if (value2)
			value1 /= value2;
		else
			id.error = CLAC_DEF_ERR;
		break;
        default:
		id.error = CLAC_PRG_ERR;
	}
	return value1;				/* return result	*/
}


/* ==================================================================== */
/*			parenthesis and help				*/
/* ==================================================================== */

/**
 * open prenthesis, push values & operators to stack
 */
static void open_bracket (void)
{
	if (id.valid == false) {		/* preceded by operator	*/
		if (par.idx < PARDEPTH_MAX) {	/* not nested too deep	*/
			par.idx ++;
			par.opx[par.idx] = op.idx + 1;
			par.vax[par.idx] = val.idx + 1;
		} else
			id.error = CLAC_STK_ERR;
	} else
		id.error = CLAC_GEN_ERR;
}

/* -------------------------------------------------------------------- */
/**
 * close prenthesis, and evaluate / pop stacks
 */
/* last parsed value, last param. flag, trigonometric mode	*/
static long long close_bracket (long long value)
{
	/* returns the value of the parenthesised expression	*/

	if (id.valid) {			/* preceded by an operator	*/
		if (par.idx > 0) {	/* prenthesis has a pair	*/
			/* calculate the value of parenthesised exp.	*/
			operation (value, LOWEST_PREDECENCE);
			value = val.buf[val.idx];
			op.idx = par.opx[par.idx] - 1;	/* restore prev	*/
			val.idx = par.vax[par.idx] - 1;
			par.idx --;

			/* next operator */
			id.valid = true;
		} else
			id.error = CLAC_PAR_ERR;
	} else
		id.error = CLAC_GEN_ERR;

	return value;
}