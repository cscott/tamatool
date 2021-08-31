/*
 * TamaTool - A cross-platform Tamagotchi P1 explorer
 *
 * Copyright (C) 2021 Jean-Christophe Rona <jc@rona.fr>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include "SDL.h"

#include "lib/tamalib.h"

#include "state.h"

#define STATE_FILE_MAGIC				"TLST"
#define STATE_FILE_VERSION				1


static uint32_t find_next_slot(void)
{
	char path[256];
	uint32_t i = 0;

	for (i = 0;; i++) {
		sprintf(path, STATE_TEMPLATE, i);
		if (!SDL_RWFromFile(path, "r")) {
			break;
		}
	}

	return i;
}

void state_find_next_name(char *path)
{
	sprintf(path, STATE_TEMPLATE, find_next_slot());
}

void state_find_last_name(char *path)
{
	uint32_t num = find_next_slot();

	if (num > 0) {
		sprintf(path, STATE_TEMPLATE, num - 1);
	} else {
		path[0] = '\0';
	}
}

struct bit_state {
	SDL_RWops *f;
	uint8_t buf;
	uint8_t num_valid;
	bool_t is_nonzero;
	uint32_t digit_count;
};

static uint32_t read_bits(struct bit_state *s, uint8_t num_bits) {
	uint32_t val = 0;
	for (uint8_t j=0; num_bits > 0; j++, num_bits--) {
		if (s->num_valid == 0) {
			SDL_RWread(s->f, &(s->buf), 1, 1);
			s->num_valid = 8;
		}
		val |= (s->buf & 1) << j;
		s->buf >>= 1;
		s->num_valid -= 1;
	}
	return val;
}

static uint32_t read_small_number(struct bit_state *s) {
	uint32_t val = 0;
	uint8_t count_bits = 0;
	while (read_bits(s, 1) == 1) {
		val += (1<<count_bits);
		count_bits = (count_bits==0) ? 1 : (count_bits<<1);
	}
	val += read_bits(s, count_bits);
	return val;
}

static uint32_t read_rle(struct bit_state *s, uint8_t num_bits) {
	uint32_t val = 0;
	for (uint8_t j=0; num_bits > 0; j++, num_bits--) {
		if (s->digit_count == 0) {
			s->is_nonzero = !s->is_nonzero;
			s->digit_count = read_small_number(s) + 1;
		}
		if (s->is_nonzero) {
			val |= (1 << j);
		}
		s->digit_count--;
	}
	return val;
}

static uint32_t read_rle_start(struct bit_state *s, uint8_t num_bits){
	bool_t first_bit = read_bits(s, 1);
	s->is_nonzero = !first_bit;
	s->digit_count = 0;
	return read_rle(s, num_bits);
}

static void write_bits(struct bit_state *s, uint32_t val, uint8_t num_bits) {
	while (num_bits > 0) {
		s->buf |= (val&1) << (s->num_valid);
		s->num_valid += 1;
		val >>= 1;
		num_bits -= 1;
		if (s->num_valid >= 8) {
			SDL_RWwrite(s->f, &(s->buf), 1, 1);
			s->buf = 0;
			s->num_valid = 0;
		}
	}
}

static void write_small_number(struct bit_state *s, uint32_t val) {
	uint8_t count_bits = 0;
	while (val >= (1<<count_bits)) {
		val -= (1<<count_bits);
		count_bits = (count_bits == 0) ? 1 : (count_bits << 1);
		write_bits(s, 1, 1);
	}
	write_bits(s, 0, 1);
	// now use count_bits to represent the number.
	write_bits(s, val, count_bits);
}

static void write_rle(struct bit_state *s, uint32_t val, uint8_t num_bits) {
	while (num_bits > 0) {
		bool_t this_bit = (val & 1);
		if (this_bit == s->is_nonzero) {
			s->digit_count++;
		} else {
			write_small_number(s, s->digit_count-1);
			s->is_nonzero = this_bit;
			s->digit_count = 1;
		}
		val >>= 1;
		num_bits -= 1;
	}
}

static void write_rle_start(struct bit_state *s, uint32_t val, uint8_t num_bits) {
	s->is_nonzero = (val & 1);
	s->digit_count = 1;
	write_bits(s, val, 1); // starting state
	write_rle(s, val>>1, num_bits-1);
}

static void flush_bits(struct bit_state *s) {
	if (s->num_valid > 0) {
		write_bits(s, 0, 8); // flush last bits
	}
}

static void write_rle_flush(struct bit_state *s) {
	write_small_number(s, s->digit_count-1);
	flush_bits(s);
}

void state_save(char *path, bool_t small)
{
	SDL_RWops *f;
	state_t *state;
	uint8_t buf[4];
	uint32_t num = 0;
	uint32_t i;

	state = tamalib_get_state();

	f = SDL_RWFromFile(path, "w");
	if (f == NULL) {
		fprintf(stderr, "FATAL: Cannot create state file \"%s\" !\n", path);
		return;
	}

	struct bit_state bs = { f, 0, 0 };
	if (small) {
		write_bits(&bs, *(state->pc), 13);
		write_bits(&bs, 1, 1); // This marks it as a "small" format file
		write_rle_start(&bs, *(state->x), 12);
		write_rle(&bs, *(state->y), 12);
		write_rle(&bs, *(state->a), 4);
		write_rle(&bs, *(state->b), 4);
		write_rle(&bs, *(state->np), 5);
		write_rle(&bs, *(state->sp), 8);
		write_rle(&bs, *(state->flags), 4);
		uint32_t tick_base = *(state->tick_counter);
		write_rle(&bs, tick_base - *(state->clk_timer_timestamp), 32);
		write_rle(&bs, tick_base - *(state->prog_timer_timestamp), 32);
		write_rle(&bs, *(state->prog_timer_enabled), 1);
		write_rle(&bs, *(state->prog_timer_data), 8);
		write_rle(&bs, *(state->prog_timer_rld), 8);
		write_rle(&bs, *(state->call_depth), 32);
		for (i = 0; i < INT_SLOT_NUM; i++) {
			write_rle(&bs, state->interrupts[i].factor_flag_reg, 4);
			write_rle(&bs, state->interrupts[i].mask_reg, 4);
			write_rle(&bs, state->interrupts[i].triggered, 1);
		}
		// Write out RAM; RLE-encode
		for (i = 0; i < MEMORY_SIZE; i++) {
			write_rle(&bs, state->memory[i], 4);
		}
		write_rle_flush(&bs);
		SDL_RWclose(f);
		return;
	}

	/* First the magic, then the version, and finally the fields of
	 * the state_t struct written as u8, u16 little-endian or u32
	 * little-endian following the struct order
	 */
	buf[0] = (uint8_t) STATE_FILE_MAGIC[0];
	buf[1] = (uint8_t) STATE_FILE_MAGIC[1];
	buf[2] = (uint8_t) STATE_FILE_MAGIC[2];
	buf[3] = (uint8_t) STATE_FILE_MAGIC[3];
	num += SDL_RWwrite(f, buf, 4, 1);

	buf[0] = STATE_FILE_VERSION & 0xFF;
	num += SDL_RWwrite(f, buf, 1, 1);

	/* All fields are written as u8, u16 little-endian or u32 little-endian following the struct order */
	buf[0] = *(state->pc) & 0xFF;
	buf[1] = (*(state->pc) >> 8) & 0x1F;
	num += SDL_RWwrite(f, buf, 2, 1);

	buf[0] = *(state->x) & 0xFF;
	buf[1] = (*(state->x) >> 8) & 0xF;
	num += SDL_RWwrite(f, buf, 2, 1);

	buf[0] = *(state->y) & 0xFF;
	buf[1] = (*(state->y) >> 8) & 0xF;
	num += SDL_RWwrite(f, buf, 2, 1);

	buf[0] = *(state->a) & 0xF;
	num += SDL_RWwrite(f, buf, 1, 1);

	buf[0] = *(state->b) & 0xF;
	num += SDL_RWwrite(f, buf, 1, 1);

	buf[0] = *(state->np) & 0x1F;
	num += SDL_RWwrite(f, buf, 1, 1);

	buf[0] = *(state->sp) & 0xFF;
	num += SDL_RWwrite(f, buf, 1, 1);

	buf[0] = *(state->flags) & 0xF;
	num += SDL_RWwrite(f, buf, 1, 1);

	buf[0] = *(state->tick_counter) & 0xFF;
	buf[1] = (*(state->tick_counter) >> 8) & 0xFF;
	buf[2] = (*(state->tick_counter) >> 16) & 0xFF;
	buf[3] = (*(state->tick_counter) >> 24) & 0xFF;
	num += SDL_RWwrite(f, buf, 4, 1);

	buf[0] = *(state->clk_timer_timestamp) & 0xFF;
	buf[1] = (*(state->clk_timer_timestamp) >> 8) & 0xFF;
	buf[2] = (*(state->clk_timer_timestamp) >> 16) & 0xFF;
	buf[3] = (*(state->clk_timer_timestamp) >> 24) & 0xFF;
	num += SDL_RWwrite(f, buf, 4, 1);

	buf[0] = *(state->prog_timer_timestamp) & 0xFF;
	buf[1] = (*(state->prog_timer_timestamp) >> 8) & 0xFF;
	buf[2] = (*(state->prog_timer_timestamp) >> 16) & 0xFF;
	buf[3] = (*(state->prog_timer_timestamp) >> 24) & 0xFF;
	num += SDL_RWwrite(f, buf, 4, 1);

	buf[0] = *(state->prog_timer_enabled) & 0x1;
	num += SDL_RWwrite(f, buf, 1, 1);

	buf[0] = *(state->prog_timer_data) & 0xFF;
	num += SDL_RWwrite(f, buf, 1, 1);

	buf[0] = *(state->prog_timer_rld) & 0xFF;
	num += SDL_RWwrite(f, buf, 1, 1);

	buf[0] = *(state->call_depth) & 0xFF;
	buf[1] = (*(state->call_depth) >> 8) & 0xFF;
	buf[2] = (*(state->call_depth) >> 16) & 0xFF;
	buf[3] = (*(state->call_depth) >> 24) & 0xFF;
	num += SDL_RWwrite(f, buf, 4, 1);

	for (i = 0; i < INT_SLOT_NUM; i++) {
		buf[0] = state->interrupts[i].factor_flag_reg & 0xF;
		num += SDL_RWwrite(f, buf, 1, 1);

		buf[0] = state->interrupts[i].mask_reg & 0xF;
		num += SDL_RWwrite(f, buf, 1, 1);

		buf[0] = state->interrupts[i].triggered & 0x1;
		num += SDL_RWwrite(f, buf, 1, 1);
	}

	for (i = 0; i < MEMORY_SIZE; i++) {
		buf[0] = state->memory[i] & 0xF;
		num += SDL_RWwrite(f, buf, 1, 1);
	}

	if (num != (17 + INT_SLOT_NUM * 3 + MEMORY_SIZE)) {
		fprintf(stderr, "FATAL: Failed to write to state file \"%s\" %u %u !\n", path, num, (23 + INT_SLOT_NUM * 3 + MEMORY_SIZE));
	}

	SDL_RWclose(f);
}

void state_debug(void) {
	state_t *state;
	state = tamalib_get_state();
	fprintf(stderr, "PC: 0x%04X\n", *(state->pc));
	fprintf(stderr, "X:  0x%03X\n", *(state->x));
	fprintf(stderr, "Y:  0x%03X\n", *(state->y));
	fprintf(stderr, "A:  0x%01X\n", *(state->a));
	fprintf(stderr, "B:  0x%01X\n", *(state->b));
	fprintf(stderr, "NP: 0x%02X\n", *(state->np));
	fprintf(stderr, "SP: 0x%02X\n", *(state->sp));
	fprintf(stderr, "FL: 0x%01X\n", *(state->flags));
	fprintf(stderr, "tick: 0x%04X\n", *(state->tick_counter));
	fprintf(stderr, "clk:  0x%04X\n", *(state->clk_timer_timestamp));
	fprintf(stderr, "prog: 0x%04X\n", *(state->prog_timer_timestamp));
	fprintf(stderr, "EN:   0x%01X\n", *(state->prog_timer_enabled));
	fprintf(stderr, "DATA: 0x%02X\n", *(state->prog_timer_data));
	fprintf(stderr, "RLD:  0x%02X\n", *(state->prog_timer_rld));
	fprintf(stderr, "call depth:  0x%04X\n", *(state->call_depth));
	fprintf(stderr, "\n");
	for (int i = 0; i < INT_SLOT_NUM; i++) {
		fprintf(stderr, "INT %X FLAG 0x%01X\n",
				i, state->interrupts[i].factor_flag_reg);
		fprintf(stderr, "INT %X MASK 0x%01X\n",
				i, state->interrupts[i].mask_reg);
		fprintf(stderr, "INT %X TRIG 0x%01X\n",
				i, state->interrupts[i].triggered);
	}
	fprintf(stderr, "\n");
	for (int i = 0; i<MEMORY_SIZE; i+=64) {
		bool_t saw_nonzero = SDL_FALSE;
		for (int j = 0; j<64; j++) {
			if (state->memory[i+j] != 0) {
				saw_nonzero = SDL_TRUE;
				break;
			}
		}
		if (!saw_nonzero) {
			continue; // skip this line, for conciseness
		}
		fprintf(stderr, "%03X: ", i);
		for (int j = 0; j<64; j++) {
			fprintf(stderr, "%01X", state->memory[i+j]);
		}
		fprintf(stderr, "\n");
	}
}

void state_load(char *path)
{
	SDL_RWops *f;
	state_t *state;
	uint8_t buf[4];
	uint32_t num = 0;
	uint32_t i;

	state = tamalib_get_state();

	f = SDL_RWFromFile(path, "r");
	if (f == NULL) {
		fprintf(stderr, "FATAL: Cannot open state file \"%s\" !\n", path);
		return;
	}

	struct bit_state bs = { f, 0, 0 };
	*(state->pc) = read_bits(&bs, 13);
	// check whether this is a "small" format file
	if (read_bits(&bs, 1)) {
		// this is "small"
		*(state->x) = read_rle_start(&bs, 12);
		*(state->y) = read_rle(&bs, 12);
		*(state->a) = read_rle(&bs, 4);
		*(state->b) = read_rle(&bs, 4);
		*(state->np) = read_rle(&bs, 5);
		*(state->sp) = read_rle(&bs, 8);
		*(state->flags) = read_rle(&bs, 4);
		uint32_t tick_base = *(state->tick_counter);
		*(state->clk_timer_timestamp) = tick_base - read_rle(&bs, 32);
		*(state->prog_timer_timestamp) = tick_base - read_rle(&bs, 32);
		*(state->prog_timer_enabled) = read_rle(&bs, 1);
		*(state->prog_timer_data) = read_rle(&bs, 8);
		*(state->prog_timer_rld) = read_rle(&bs, 8);
		*(state->call_depth) = read_rle(&bs, 32);
		for (i = 0; i < INT_SLOT_NUM; i++) {
			state->interrupts[i].factor_flag_reg = read_rle(&bs, 4);
			state->interrupts[i].mask_reg = read_rle(&bs, 4);
			state->interrupts[i].triggered = read_rle(&bs, 1);
		}
		// Read RAM, RLE-encoded
		for (i = 0; i < MEMORY_SIZE; i++) {
			state->memory[i] = read_rle(&bs, 4);
		}
		SDL_RWclose(f);
		return;
	}
	// HACK to use the "long form" save format
	buf[0] = STATE_FILE_MAGIC[0];
	buf[1] = STATE_FILE_MAGIC[1];
	/* First the magic, then the version, and finally the fields of
	 * the state_t struct written as u8, u16 little-endian or u32
	 * little-endian following the struct order
	 */
	num += SDL_RWread(f, buf+2, 2, 1);
	if (buf[0] != (uint8_t) STATE_FILE_MAGIC[0] || buf[1] != (uint8_t) STATE_FILE_MAGIC[1] ||
		buf[2] != (uint8_t) STATE_FILE_MAGIC[2] || buf[3] != (uint8_t) STATE_FILE_MAGIC[3]) {
		fprintf(stderr, "FATAL: Wrong state file magic in \"%s\" !\n", path);
		return;
	}

	num += SDL_RWread(f, buf, 1, 1);
	if (buf[0] != STATE_FILE_VERSION) {
		fprintf(stderr, "FATAL: Unsupported version %u (expected %u) in state file \"%s\" !\n", buf[0], STATE_FILE_VERSION, path);
		/* TODO: Handle migration at a point */
		return;
	}

	/* All fields are read as u8, u16 little-endian or u32 little-endian following the struct order */
	num += SDL_RWread(f, buf, 2, 1);
	*(state->pc) = buf[0] | ((buf[1] & 0x1F) << 8);

	num += SDL_RWread(f, buf, 2, 1);
	*(state->x) = buf[0] | ((buf[1] & 0xF) << 8);

	num += SDL_RWread(f, buf, 2, 1);
	*(state->y) = buf[0] | ((buf[1] & 0xF) << 8);

	num += SDL_RWread(f, buf, 1, 1);
	*(state->a) = buf[0] & 0xF;

	num += SDL_RWread(f, buf, 1, 1);
	*(state->b) = buf[0] & 0xF;

	num += SDL_RWread(f, buf, 1, 1);
	*(state->np) = buf[0] & 0x1F;

	num += SDL_RWread(f, buf, 1, 1);
	*(state->sp) = buf[0];

	num += SDL_RWread(f, buf, 1, 1);
	*(state->flags) = buf[0] & 0xF;

	num += SDL_RWread(f, buf, 4, 1);
	*(state->tick_counter) = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);

	num += SDL_RWread(f, buf, 4, 1);
	*(state->clk_timer_timestamp) = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);

	num += SDL_RWread(f, buf, 4, 1);
	*(state->prog_timer_timestamp) = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);

	num += SDL_RWread(f, buf, 1, 1);
	*(state->prog_timer_enabled) = buf[0] & 0x1;

	num += SDL_RWread(f, buf, 1, 1);
	*(state->prog_timer_data) = buf[0];

	num += SDL_RWread(f, buf, 1, 1);
	*(state->prog_timer_rld) = buf[0];

	num += SDL_RWread(f, buf, 4, 1);
	*(state->call_depth) = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);

	for (i = 0; i < INT_SLOT_NUM; i++) {
		num += SDL_RWread(f, buf, 1, 1);
		state->interrupts[i].factor_flag_reg = buf[0] & 0xF;

		num += SDL_RWread(f, buf, 1, 1);
		state->interrupts[i].mask_reg = buf[0] & 0xF;

		num += SDL_RWread(f, buf, 1, 1);
		state->interrupts[i].triggered = buf[0] & 0x1;
	}

	for (i = 0; i < MEMORY_SIZE; i++) {
		num += SDL_RWread(f, buf, 1, 1);
		state->memory[i] = buf[0] & 0xF;
	}

	if (num != (17 + INT_SLOT_NUM * 3 + MEMORY_SIZE)) {
		fprintf(stderr, "FATAL: Failed to read from state file \"%s\" !\n", path);
	}

	SDL_RWclose(f);

	tamalib_refresh_hw();
}
