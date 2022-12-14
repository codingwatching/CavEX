#include <assert.h>

#include "../cNBT/nbt.h"

#include "region_archive.h"

bool region_archive_create(struct region_archive* ra, char* world_name,
						   w_coord_t x, w_coord_t z) {
	assert(ra && world_name);

	ra->offsets = malloc(sizeof(uint32_t) * REGION_SIZE * REGION_SIZE);

	if(!ra->offsets)
		return false;

	snprintf(ra->file_name, sizeof(ra->file_name), "%s/region/r.%i.%i.mcr",
			 world_name, x, z); // TODO
	ra->x = x;
	ra->z = z;

	FILE* f = fopen(ra->file_name, "rb");

	if(!f) {
		free(ra->offsets);
		return false;
	}

	if(!fread(ra->offsets, sizeof(uint32_t) * REGION_SIZE * REGION_SIZE, 1,
			  f)) {
		free(ra->offsets);
		fclose(f);
		return false;
	}

	fclose(f);

	ilist_regions_init_field(ra);

	return true;
}

void region_archive_destroy(struct region_archive* ra) {
	assert(ra && ra->offsets);

	free(ra->offsets);
}

bool region_archive_contains(struct region_archive* ra, w_coord_t x,
							 w_coord_t z, bool* chunk_exists) {
	assert(ra && chunk_exists);

	if(CHUNK_REGION_COORD(x) != ra->x || CHUNK_REGION_COORD(z) != ra->z) {
		*chunk_exists = false;
		return false;
	}

	int rx = x & (REGION_SIZE - 1);
	int rz = z & (REGION_SIZE - 1);

	uint32_t offset = ra->offsets[rx + rz * REGION_SIZE] >> 8;
	uint32_t sectors = ra->offsets[rx + rz * REGION_SIZE] & 0xFF;

	*chunk_exists = offset >= 2 && sectors >= 1;
	return true;
}

bool region_archive_get_blocks(struct region_archive* ra, w_coord_t x,
							   w_coord_t z, uint8_t** ids, uint8_t** metadata,
							   uint8_t** lighting) {
	assert(ra && ids && metadata && lighting);
	bool chunk_exists;
	assert(region_archive_contains(ra, x, z, &chunk_exists) && chunk_exists);

	int rx = x & (REGION_SIZE - 1);
	int rz = z & (REGION_SIZE - 1);

	uint32_t offset = ra->offsets[rx + rz * REGION_SIZE] >> 8;
	uint32_t sectors = ra->offsets[rx + rz * REGION_SIZE] & 0xFF;

	// TODO: little endian

	FILE* f = fopen(ra->file_name, "rb");

	if(!f)
		return false;

	if(fseek(f, offset * REGION_SECTOR_SIZE, SEEK_SET) != 0) {
		fclose(f);
		return false;
	}

	uint32_t length;
	if(!fread(&length, sizeof(uint32_t), 1, f)
	   || length + sizeof(uint32_t) > sectors * REGION_SECTOR_SIZE) {
		fclose(f);
		return false;
	}

	uint8_t type;
	if(!fread(&type, sizeof(uint8_t), 1, f) || type > 3) {
		fclose(f);
		return false;
	}

	void* nbt_compressed = malloc(length - 1);

	if(!nbt_compressed) {
		fclose(f);
		return false;
	}

	if(!fread(nbt_compressed, length - 1, 1, f)) {
		free(nbt_compressed);
		fclose(f);
		return false;
	}

	nbt_node* chunk = nbt_parse_compressed(nbt_compressed, length - 1);

	free(nbt_compressed);
	fclose(f);

	if(!chunk)
		return false;

	nbt_node* n_blocks = nbt_find_by_path(chunk, ".Level.Blocks");
	nbt_node* n_metadata = nbt_find_by_path(chunk, ".Level.Data");
	nbt_node* n_skyl = nbt_find_by_path(chunk, ".Level.SkyLight");
	nbt_node* n_torchl = nbt_find_by_path(chunk, ".Level.BlockLight");

	if(!n_blocks || !n_metadata || !n_skyl || !n_torchl
	   || n_blocks->type != TAG_BYTE_ARRAY || n_metadata->type != TAG_BYTE_ARRAY
	   || n_skyl->type != TAG_BYTE_ARRAY || n_torchl->type != TAG_BYTE_ARRAY
	   || n_blocks->payload.tag_byte_array.length
		   != CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT
	   || n_metadata->payload.tag_byte_array.length
		   != CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2
	   || n_skyl->payload.tag_byte_array.length
		   != CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2
	   || n_torchl->payload.tag_byte_array.length
		   != CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2) {
		nbt_free(chunk);
		return false;
	}

	*ids = malloc(CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT);
	*metadata = malloc(CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2);
	*lighting = malloc(CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT);

	memcpy(*ids, n_blocks->payload.tag_byte_array.data,
		   n_blocks->payload.tag_byte_array.length);

	memcpy(*metadata, n_metadata->payload.tag_byte_array.data,
		   n_metadata->payload.tag_byte_array.length);

	for(size_t k = 0; k < (size_t)n_skyl->payload.tag_byte_array.length * 2;
		k++) {
		uint8_t a = (n_torchl->payload.tag_byte_array.data[k / 2] & 0xF0)
			| (n_skyl->payload.tag_byte_array.data[k / 2] >> 4);
		uint8_t b = (n_torchl->payload.tag_byte_array.data[k / 2] << 4)
			| (n_skyl->payload.tag_byte_array.data[k / 2] & 0xF);
		(*lighting)[k] = (k & 1) ? a : b;
	}

	nbt_free(chunk);

	return true;
}