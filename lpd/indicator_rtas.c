/**
 * @file	indicator_rtas.c
 * @brief	RTAS indicator manipulation routines
 *
 * Copyright (C) 2012 IBM Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * @author	Vasant Hegde <hegdevasant@linux.vnet.ibm.com>
 */

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <librtas.h>

#include "indicator.h"
#include "indicator_ses.h"

/* Device tree property for indicator list */
#define RTAS_INDICATOR_DT_PATH	"/proc/device-tree/rtas/ibm,get-indices"

/* Token defination for indicator */
#define RTAS_INDICATOR_IDENT	9007
#define RTAS_INDICATOR_ATTN	9006

/* Defination for type of indicator call */
#define DYNAMIC_INDICATOR       0xFFFFFFFF

/* RTAS error buffer size */
#define RTAS_ERROR_BUF_SIZE	64

/* Get RTAS token number for given LED type */
static inline int get_rtas_token(int indicator)
{
	switch(indicator) {
	/* We use same token number for fault and checklog indicator */
	case LED_TYPE_ATTN:
	case LED_TYPE_FAULT:
		return RTAS_INDICATOR_ATTN;
	case LED_TYPE_IDENT:
		return RTAS_INDICATOR_IDENT;
	}
	return -1;
}

/**
 * parse_work_area - Parse the working buffer
 *
 * @loc		list to add new data to
 * @buf		working area to parse
 *
 * Returns :
 *	pointer to new loc_code list
 */
static struct loc_code *
parse_rtas_workarea(struct loc_code *loc, const char *buf)
{
	int	i;
	uint32_t num = be32toh(*(uint32_t *)buf);
	struct	loc_code *curr = loc;

	if (curr)
		while (curr->next)
			curr = curr->next;

	buf += sizeof(uint32_t);
	for (i = 0; i < num; i++) {
		if (!curr) {
			curr = calloc(1, sizeof(struct loc_code));
			loc = curr;
		} else {
			curr->next = calloc(1, sizeof(struct loc_code));
			curr = curr->next;
		}
		if (!curr) {
			log_msg("Out of memory");
			/* free up previously allocated memory */
			free_indicator_list(loc);
			return NULL;
		}

		/*
		 * NOTE: Location code length and location code string combined
		 * is given as the input to LED indicator related rtas calls. So
		 * the buffer representation of the location code length and
		 * location code string must be PAPR compliant and big endian at
		 * all times. Convert the location code length to host format when
		 * ever required on the need basis.
		 */
		curr->index = be32toh(*(uint32_t *)buf);
		buf += sizeof(uint32_t);
		/* Includes NULL char */
		curr->length = be32toh(*(uint32_t *)buf);
		buf += sizeof(uint32_t);
		/* Corrupted work area */
		if (curr->length > LOCATION_LENGTH) {
			free_indicator_list(loc);
			return NULL;
		}
		strncpy(curr->code, buf, curr->length);
		buf += curr->length;
		curr->code[curr->length] = '\0';
		curr->length = strlen(curr->code) + 1;
		curr->length = htobe32(curr->length);
		curr->type = TYPE_RTAS;
		curr->next = NULL;
	}

	return loc;
}

/**
 * librtas_error - Check for librtas specific return codes
 *
 * @Note:
 *	librtas_error is based on librtas_error.c in powerpc-util package.
 *	@author Nathan Fontenot <nfont@austin.ibm.com>
 *
 * This will check the error value for a librtas specific return code
 * and fill in the buffer with the appropriate error message.
 *
 * @error	return code from librtas
 * @buf		buffer to fill with error string
 * @size	size of "buffer"
 *
 * Returns :
 *	nothing
 */
static void
librtas_error(int error, char *buf, size_t size)
{
	switch (error) {
	case RTAS_KERNEL_INT:
		snprintf(buf, size, "No kernel interface to firmware");
		break;
	case RTAS_KERNEL_IMP:
		snprintf(buf, size, "No kernel implementation of function");
		break;
	case RTAS_PERM:
		snprintf(buf, size, "Non-root caller");
		break;
	case RTAS_NO_MEM:
		snprintf(buf, size, "Out of heap memory");
		break;
	case RTAS_NO_LOWMEM:
		snprintf(buf, size, "Kernel out of low memory");
		break;
	case RTAS_FREE_ERR:
		snprintf(buf, size, "Attempt to free nonexistent RMO buffer");
		break;
	case RTAS_TIMEOUT:
		snprintf(buf, size, "RTAS delay exceeded specified timeout");
		break;
	case RTAS_IO_ASSERT:
		snprintf(buf, size, "Unexpected librtas I/O error");
		break;
	case RTAS_UNKNOWN_OP:
		snprintf(buf, size, "No firmware implementation of function");
		break;
	default:
		snprintf(buf, size, "Unknown librtas error %d", error);
	}
}

/**
 * get_rtas_list - Gets rtas indicator list
 *
 * @indicator	identification or attention indicator
 * @loc		pointer to loc_code structure
 *
 * Returns :
 *	rtas call return value
 */
static int
get_rtas_indices(int indicator, struct loc_code **loc)
{
	int	rc;
	int	index = 1;
	int	next_index;
	int	rtas_token;
	char	workarea[BUF_SIZE];
	char	err_buf[RTAS_ERROR_BUF_SIZE];
	struct	loc_code *list = NULL;

	rtas_token = get_rtas_token(indicator);
	if (rtas_token == -1)
		return -3; /* Indicator type not supported */

	do {
		rc = rtas_get_indices(0, rtas_token, workarea, BUF_SIZE,
				      index, &next_index);
		switch (rc) {
		case 1:		/* more data available */
			index = next_index;
			/* fall through */
		case 0:		/* success */
			list = parse_rtas_workarea(list, workarea);
			if (!list)
				return -99; /* Out of heap memory */
			break;
		case -1:	/* hardware error */
			log_msg("Hardware error retrieving indicator indices");
			free_indicator_list(list);
			break;
		case RTAS_UNKNOWN_OP:
			/* Yes, this is a librtas return code but it should
			 * be treated the same as a -3 return code, both
			 * indicate that functionality is not supported
			 */
			librtas_error(rc, err_buf, RTAS_ERROR_BUF_SIZE);
			/* fall through */
		case -3:	/* indicator type not supported. */
			log_msg("The %s indicators are not supported on this "
				"system", get_indicator_desc(indicator));

			if (rc == RTAS_UNKNOWN_OP)
				log_msg(",\n%s", err_buf);

			free_indicator_list(list);
			break;
		case -4:	/* list changed; start over */
			free_indicator_list(list);
			list = NULL;
			index = 1;
			break;
		default:
			librtas_error(rc, err_buf, RTAS_ERROR_BUF_SIZE);
			log_msg("Could not retrieve data for %s "
				"indicators,\n%s",
				get_indicator_desc(indicator), err_buf);
			free_indicator_list(list);
			break;
		}

	} while ((rc == 1) || (rc == -4));

	*loc = list;
	return rc;
}

/**
 * get_rtas_sensor - Retrieve a sensor value from rtas
 *
 * Call the rtas_get_sensor or rtas_get_dynamic_sensor librtas calls,
 * depending on whether the index indicates that the sensor is dynamic.
 *
 * @indicator	identification or attention indicator
 * @loc		location code of the sensor
 * @state	return sensor state for the given loc
 *
 * Returns :
 *	rtas call return code
 */
static int
get_rtas_sensor(int indicator, struct loc_code *loc, int *state)
{
	int	rc;
	int	rtas_token;
	char	err_buf[RTAS_ERROR_BUF_SIZE];

	rtas_token = get_rtas_token(indicator);
	if (rtas_token == -1)
		return -3; /* No such sensor implemented */

	if (loc->index == DYNAMIC_INDICATOR)
		rc = rtas_get_dynamic_sensor(rtas_token,
					     (void *)loc, state);
	else
		rc = rtas_get_sensor(rtas_token, loc->index, state);

	switch (rc) {
	case 0:	/*success  */
		break;
	case -1:
		log_msg("Hardware error retrieving the indicator at %s",
			loc->code);
		break;
	case -2:
		log_msg("Busy while retrieving indicator at %s. "
			"Try again later", loc->code);
		break;
	case -3:
		log_msg("The indicator at %s is not implemented", loc->code);
		break;
	default:
		librtas_error(rc, err_buf, RTAS_ERROR_BUF_SIZE);

		log_msg("Could not get %ssensor %s indicators,\n%s",
			(loc->index == DYNAMIC_INDICATOR) ? "dynamic " : "",
			get_indicator_desc(indicator), err_buf);
		break;
	}

	return rc;
}

/**
 * set_rtas_indicator - Set rtas indicator
 *
 * Call the rtas_set_indicator or rtas_set_dynamic_indicator librtas calls,
 * depending on whether the index indicates that the indicator is dynamic.
 *
 * @indicator	identification or attention indicator
 * @loc		location code of rtas indicator
 * @new_value	value to update indicator to
 *
 * Returns :
 *	rtas call return code
 */
static int
set_rtas_indicator(int indicator, struct loc_code *loc, int new_value)
{
	int	rc;
	int	rtas_token;
	char	err_buf[RTAS_ERROR_BUF_SIZE];

	rtas_token = get_rtas_token(indicator);
	if (rtas_token == -1)
		return -3; /* No such sensor implemented */

	if (loc->index == DYNAMIC_INDICATOR)
		rc = rtas_set_dynamic_indicator(rtas_token,
						new_value, (void *)loc);
	else
		rc = rtas_set_indicator(rtas_token, loc->index, new_value);

	switch (rc) {
	case 0:	/*success  */
		break;
	case -1:
		log_msg("Hardware error while setting the indicator at %s",
			loc->code);
		break;
	case -2:
		log_msg("Busy while setting the indicator at %s. "
			"Try again later", loc->code);
		break;
	case -3:
		log_msg("The indicator at %s is not implemented", loc->code);
		break;
	default:
		librtas_error(rc, err_buf, RTAS_ERROR_BUF_SIZE);

		log_msg("Could not set %ssensor %s indicators,\n%s",
			(loc->index == DYNAMIC_INDICATOR) ? "dynamic " : "",
			get_indicator_desc(indicator), err_buf);
		break;
	}

	return rc;
}

/*
 * rtas_indicator_probe - Probe indicator support on RTAS based platform
 *
 * Returns:
 *   0 if indicator is supported, else -1
 */
static int
rtas_indicator_probe(bool platform_only)
{
        if (platform_only)
                system_scan_flag = 1 << TYPE_RTAS;
        else
                system_scan_flag = 1 << TYPE_ALL | 1 << TYPE_RTAS | 1 << TYPE_SES;

	return 0;
}

/**
 * get_rtas_indicator_mode - Gets the service indicator operating mode
 *
 * Note: There is no defined property in PAPR+ to determine the indicator
 *	 operating mode. There is some work being done to get property
 *	 into PAPR. When that is done we will check for that property.
 *
 *	 At present, we will query RTAS fault indicators. It should return
 *	 at least one fault indicator, that is check log indicator. If only
 *	 one indicator is returned, then Guiding Light mode else Light Path
 *	 mode.
 *
 * Returns :
 *	operating mode value
 */
static int
get_rtas_indicator_mode(void)
{
	int	rc;
	struct	loc_code *list = NULL;

	rc = get_rtas_indices(LED_TYPE_FAULT, &list);
	if (rc)
		return -1;

	if (!list)	/* failed */
		return -1;
	else if (!list->next)
		operating_mode = LED_MODE_GUIDING_LIGHT;
	else
		operating_mode = LED_MODE_LIGHT_PATH;

	free_indicator_list(list);
	return 0;
}

/**
 * get_rtas_indicator_list - Build indicator list of given type
 *
 * @indicator	identification or attention indicator
 * @list	loc_code structure
 *
 * Returns :
 *	0 on success, !0 otherwise
 */
static int
get_rtas_indicator_list(int indicator, struct loc_code **list)
{
	int	rc;

	/*
	 * Get RTAS indicator list
	 * NB: Check the system_scan_flag if the scanning of platform
	 * indicators needs to be prevented.
	 */
	rc = get_rtas_indices(indicator, list);
	if (rc)
		return rc;

	/* FRU fault indicators are not supported in Guiding Light mode */
	if (indicator == LED_TYPE_FAULT &&
	    operating_mode == LED_MODE_GUIDING_LIGHT)
		return rc;

	/* SES indicators */
	if (system_scan_flag & (1 << TYPE_SES))
		get_ses_indices(indicator, list);

	return rc;
}

/**
 * get_rtas_indicator_state - Retrieve the current state for an indicator
 *
 * Call the appropriate routine for retrieving indicator values based on the
 * type of indicator.
 *
 * @indicator	identification or attention indicator
 * @loc		location code of the sensor
 * @state	return location for the sensor state
 *
 * Returns :
 *	indicator return code
 */
static int
get_rtas_indicator_state(int indicator, struct loc_code *loc, int *state)
{
	switch (loc->type) {
	case TYPE_RTAS:
		return get_rtas_sensor(indicator, loc, state);
	case TYPE_SES:
		return get_ses_indicator(indicator, loc, state);
	default:
		break;
	}

	return -1;
}

/**
 * set_rtas_indicator_state - Set an indicator to a new state (on or off)
 *
 * Call the appropriate routine for setting indicators based on the type
 * of indicator.
 *
 * @indicator	identification or attention indicator
 * @loc		location code of rtas indicator
 * @new_value	value to update indicator to
 *
 * Returns :
 *	indicator return code
 */
static int
set_rtas_indicator_state(int indicator, struct loc_code *loc, int new_value)
{
	switch (loc->type) {
	case TYPE_RTAS:
		return set_rtas_indicator(indicator, loc, new_value);
	case TYPE_SES:
		return set_ses_indicator(indicator, loc, new_value);
	default:
		break;
	}

	return -1;
}

struct platform rtas_platform = {
	.name			= "rtas",
	.probe			= rtas_indicator_probe,
	.get_indicator_mode	= get_rtas_indicator_mode,
	.get_indicator_list	= get_rtas_indicator_list,
	.get_indicator_state	= get_rtas_indicator_state,
	.set_indicator_state	= set_rtas_indicator_state,
};
