/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 1994, Sun Microsystems, Inc.
 * Copyright (c) 1994, Nihon Sun Microsystems K.K.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <euc.h>
#include "japanese.h"

/*
 * struct _cv_state; to keep status
 */
struct _icv_state {
	int	_st_cset;
	int	_st_stat;
};

extern int errno;

/*
 * Open; called from iconv_open(); as taken unchanged from @(#)ISO-2022-JP%SJIS.
 */
void *
_icv_open()
{
	struct _icv_state *st;

	if ((st = (struct _icv_state *)malloc(sizeof(struct _icv_state)))
									== NULL)
		return ((void *)ERR_RETURN);

	st->_st_cset = CS_0;
	st->_st_stat = ST_INIT;

	return (st);
}


/*
 * Close; called from iconv_close();  as taken unchanged from @(#)ISO-2022-JP%SJIS.
 */
void
_icv_close(struct _icv_state *st)
{
	free(st);
}



/*
 * Actual conversion; called from iconv()
 */
size_t
_icv_iconv(struct _icv_state *st, char **inbuf, size_t *inbytesleft,
				char **outbuf, size_t *outbytesleft)
{
	int				cset, stat;
	unsigned char	*op, ic;
	char			*ip;
	size_t			ileft, oleft;
	size_t			retval;

	cset = st->_st_cset;
	stat = st->_st_stat;

	/*
	 * If (inbuf == 0 || *inbuf == 0) then this conversion is
	 * placed into initial state.
	 */
	if ((inbuf == 0) || (*inbuf == 0)) {
		cset = CS_0;
		stat = ST_INIT;
		op = (unsigned char *)*outbuf;
		oleft = *outbytesleft;
		retval = 0;
		goto ret2;
	}

	ip = *inbuf;
	op = (unsigned char *)*outbuf;
	ileft = *inbytesleft;
	oleft = *outbytesleft;
	/* Everything down to here was taken unchanged from  @(#)ISO-2022-JP%SJIS.
	   =======================================================================

	 *
	 * Main loop; basically 1 loop per 1 input byte
	 */

	while (ileft > 0)
	{
		GET(ic);
	/*
		If the char is one of the following [ / ] { | } then convert
		it to its corresponding value. In all other cases if the char
		is greater than octal \178 ( ie a high bit char) convert it
		to an underscore (_), as it has no mapping to 7 bit ASCII.
		Otrherwise the char is the same in both cose sets.
	*/
		if ( ic == '[' )
				ic = '_';
		else if ( ic == '\134' )
				ic = '_';
		else if ( ic == ']' )
				ic = '_';
		else if ( ic == '{' )
				ic = '_';
		else if ( ic == '|' )
				ic = '_';
		else if ( ic == '}' )
				ic = '_';
		else if ( ic == '~' )
				ic = '_';
		else if ( ic == '\100' )
				ic = '_';
		else if ( ic == 167 )
				ic = '\100';
		else if ( ic == 223 )
				ic = '~';
		else if ( ic == 220 )
				ic = ']';
		else if ( ic == 196 )
				ic = '[';
		else if ( ic == 214 )
				ic = '\134';
		else if ( ic == 252 )
				ic = '}';
		else if ( ic == 228 )
				ic = '{';
		else if ( ic == 246 )
				ic = '|';
		else if (ic > '\177')
					ic = '_';







/*		switch ( ic )
		{
			case '[' :
				ic = '_';
				break;
			case '\134' :
				ic = '_';
				break;
			case ']' :
				ic = '_';
				break;
			case '{' :
				ic = '_';
				break;
			case '|' :
				ic = '_';
				break;
			case '}' :
				ic = '_';
				break;
			case '~' :
				ic = '_';
				break;
			case '\100' :
				ic = '_';
				break;
			case 167 :
				ic = '\100';
				break;
			case 223 :
				ic = '~';
				break;
			case 220 :
				ic = ']';
				break;
			case 196 :
				ic = '[';
				break;
			case 214 :
				ic = '\134';
				break;
			case 252 :
				ic = '}';
				break;
			case 228 :
				ic = '{';
				break;
			case 246 :
				ic = '|';
				break;
			default :
				if (ic > '\177')
					ic = '_';
				break;
		} */

		PUT(ic);
	/*
		Put the converted character into the output buffer, and decrement
		the count of chars left in both the in and out buffers.
		If we have no space left in the out buffer, but we have no reached
		the end of the input buffer. We return what we have, and set the
		errno (Error) to E2BIG.
	*/
		if ((oleft < 1)	 && (ileft > 0))
		{
			errno = E2BIG;
			retval = ERR_RETURN;
			goto ret;
		}


	}
/*
We only get here if the end of the in buffer has been reached, we therefore return the
value 0 to denote that we have sucesfully converted the inbuffer.
*/
	retval = ileft;

/*  Taken unchanged from   @(#)ISO-2022-JP%SJIS.  */

ret:
	st->_st_cset = cset;
	st->_st_stat = stat;
	*inbuf = ip;
	*inbytesleft = ileft;
ret2:
	*outbuf = (char *)op;
	*outbytesleft = oleft;

	return (retval);
}
