/*
 * DisketteRecover - JSON serialisation for the CLI and the viewer.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>

#include "dr_internal.h"

static const char *crc_name(dr_crc_state s)
{
	switch (s) {
	case DR_CRC_OK:     return "ok";
	case DR_CRC_BAD:    return "bad";
	default:            return "absent";
	}
}

static const char *enc_name(dr_encoding e)
{
	switch (e) {
	case DR_ENC_ISO_MFM: return "iso_mfm";
	case DR_ENC_ISO_FM:  return "iso_fm";
	default:             return "unsupported";
	}
}

static const char *ev_name(int e)
{
	switch (e) {
	case DR_EV_WEAKBIT:   return "weak";
	case DR_EV_FLUX:      return "flux";
	case DR_EV_VIOLATION: return "violation";
	default:              return "none";
	}
}

static void json_str(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			fprintf(f, "\\%c", *s);
		else if ((unsigned char)*s < 0x20)
			fprintf(f, "\\u%04x", *s);
		else
			fputc(*s, f);
	}
	fputc('"', f);
}

void dr_json_scan(dr_ctx *c, FILE *f)
{
	int i;

	fprintf(f, "{\"image\":");
	json_str(f, dr_path(c));
	fprintf(f, ",\"tracks\":%d,\"sides\":%d,\"sector_count\":%d,"
	           "\"first_bad\":%d,\"dirty\":%d,\"sectors\":[",
	        dr_tracks(c), dr_sides(c), c->nsectors, dr_first_bad(c),
	        c->dirty);

	for (i = 0; i < c->nsectors; i++) {
		const dr_sector *s = &c->sectors[i];
		fprintf(f, "%s{\"index\":%d,\"track\":%d,\"side\":%d,\"order\":%d,"
		           "\"id\":%d,\"cyl_id\":%d,\"head_id\":%d,\"size\":%d,"
		           "\"encoding\":\"%s\",\"bitrate\":%d,"
		           "\"header_crc\":\"%s\",\"data_crc\":\"%s\","
		           "\"stored_header_crc\":%u,\"stored_data_crc\":%u,"
		           "\"datamark\":%d,\"start_cell\":%d,\"data_cell\":%d,"
		           "\"end_cell\":%d}",
		        i ? "," : "", i, s->track, s->side, s->order,
		        s->sector_id, s->cylinder_id, s->head_id, s->sector_size,
		        enc_name(s->encoding), s->bitrate,
		        crc_name(s->header_crc), crc_name(s->data_crc),
		        s->stored_header_crc, s->stored_data_crc,
		        s->datamark, s->start_cell, s->data_cell, s->end_cell);
	}
	fprintf(f, "]}");
}

void dr_json_view(dr_ctx *c, dr_view *v, FILE *f)
{
	int i, k;

	(void)c;

	fprintf(f, "{\"sector_index\":%d,\"track\":%d,\"side\":%d,\"id\":%d,"
	           "\"encoding\":\"%s\",\"field\":\"%s\","
	           "\"sector_size\":%d,\"bitrate\":%d,"
	           "\"msg_len\":%d,\"msg_bits\":%d,\"first_bit\":%d,"
	           "\"data_offset\":%d,\"data_len\":%d,"
	           "\"syndrome\":%u,\"stored_crc\":%u,\"computed_crc\":%u,"
	           "\"crc_valid\":%s,\"flux\":%s,\"stride\":%d,"
	           "\"base_cell\":%d,\"track_len\":%d,\"model\":",
	        v->sector_index, v->sect.track, v->sect.side, v->sect.sector_id,
	        enc_name(v->encoding), v->field,
	        v->sect.sector_size, v->sect.bitrate,
	        v->msg_len, v->msg_bits, v->first_bit,
	        v->data_offset, v->data_len,
	        v->syndrome, v->stored_crc, v->computed_crc,
	        v->syndrome ? "false" : "true",
	        v->flux_available ? "true" : "false",
	        v->stride, v->base_cell,
	        ((HXCFE_SIDE *)v->side)->tracklen);
	json_str(f, v->model);

	/* bytes */
	fprintf(f, ",\"bytes\":[");
	for (i = 0; i < v->nbytes; i++) {
		fprintf(f, "%s{\"i\":%d,\"cell\":%d,\"v\":%u,\"role\":\"%s\",\"p\":[",
		        i ? "," : "", i, v->bytes[i].cell, v->bytes[i].value,
		        v->bytes[i].role);
		for (k = 0; k < 8; k++)
			fprintf(f, "%s%.6g", k ? "," : "", v->bytes[i].p_err[k]);
		fprintf(f, "]}");
	}
	fprintf(f, "]");

	/* cells, as parallel arrays to keep the payload small */
	fprintf(f, ",\"cells\":{\"first\":%d,\"n\":%d,\"state\":\"",
	        v->first_cell, v->ncells);
	for (i = 0; i < v->ncells; i++)
		fputc(v->cells[i].state ? '1' : '0', f);
	fprintf(f, "\",\"weak\":\"");
	for (i = 0; i < v->ncells; i++)
		fputc(v->cells[i].weak ? '1' : '0', f);
	fprintf(f, "\",\"perr\":[");
	for (i = 0; i < v->ncells; i++)
		fprintf(f, "%s%.4g", i ? "," : "", v->cells[i].p_err);
	fprintf(f, "],\"bin\":[");
	for (i = 0; i < v->ncells; i++)
		fprintf(f, "%s%d", i ? "," : "", v->cells[i].bin);
	fprintf(f, "],\"icells\":[");
	for (i = 0; i < v->ncells; i++)
		fprintf(f, "%s%.4g", i ? "," : "", v->cells[i].interval_cells);
	fprintf(f, "],\"ticks\":[");
	for (i = 0; i < v->ncells; i++)
		fprintf(f, "%s%d", i ? "," : "", v->cells[i].interval_ticks);
	fprintf(f, "],\"evidence\":[");
	for (i = 0; i < v->ncells; i++)
		fprintf(f, "%s\"%s\"", i ? "," : "", ev_name(v->cells[i].evidence));
	fprintf(f, "]}}");
}

void dr_json_candidates(const dr_view *v, const dr_repair_result *r,
                        int offset, int limit, FILE *f)
{
	int i, k;

	if (offset < 0)
		offset = 0;
	if (limit <= 0 || offset + limit > r->count)
		limit = r->count - offset;
	if (limit < 0)
		limit = 0;

	{
		double margin = 0.0;

		if (r->count > 1 && r->list[1].rel_likelihood > 0.0)
			margin = 1.0 / r->list[1].rel_likelihood;

		fprintf(f, "{\"count\":%d,\"offset\":%d,\"pool\":%d,"
		           "\"searched_weight\":%d,\"truncated\":%s,"
		           "\"margin\":%.6g,\"flux\":%s,\"candidates\":[",
		        r->count, offset, r->npool, r->searched_weight,
		        r->truncated ? "true" : "false", margin,
		        v->flux_available ? "true" : "false");
	}

	for (i = offset; i < offset + limit; i++) {
		const dr_candidate *cd = &r->list[i];
		uint8_t *m = dr_candidate_message(v, cd);

		fprintf(f, "%s{\"rank\":%d,\"weight\":%d,\"loglik\":%.6g,"
		           "\"rel\":%.6g,\"bits\":[",
		        i > offset ? "," : "", i, cd->weight,
		        cd->log_likelihood, cd->rel_likelihood);
		for (k = 0; k < cd->weight; k++)
			fprintf(f, "%s%d", k ? "," : "", cd->bits[k]);
		fprintf(f, "],\"edits\":[");
		for (k = 0; k < cd->weight; k++) {
			int p = cd->bits[k];
			int byte = p >> 3;
			int bit = p & 7;
			int cc, dc;
			dr_bit_cells(v->encoding, byte * v->stride, bit, &cc, &dc);
			fprintf(f, "%s{\"bit\":%d,\"byte\":%d,\"bitpos\":%d,"
			           "\"from\":%d,\"to\":%d,\"clock_cell\":%d,"
			           "\"data_cell\":%d,\"perr\":%.5g}",
			        k ? "," : "", p, byte, bit,
			        cd->before[k], !cd->before[k],
			        v->first_cell + cc, v->first_cell + dc,
			        v->bit_perr[p]);
		}
		fprintf(f, "],\"data\":\"");
		if (m) {
			for (k = 0; k < v->data_len; k++)
				fprintf(f, "%02x", m[v->data_offset + k]);
		}
		fprintf(f, "\"}");
		free(m);
	}
	fprintf(f, "]}");
}
