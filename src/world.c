#include "common.h"

#define WORLD_FALLING_CLASSIC

struct update_entry {
	uint8_t cx, cy, cz;
	uint32_t time;
	struct update_entry* next;
};
typedef struct update_entry update_entry_t;

static uint8_t level_opaque_height[LEVEL_LZ][LEVEL_LX];
static uint8_t level_faces[LEVEL_LY][LEVEL_LZ][LEVEL_LX];
static uint16_t level_has_vis_blocks[LEVEL_LY>>2][LEVEL_LZ>>2][LEVEL_LX>>2];
static uint32_t level_time = 0;
static uint32_t target_level_time = 0;
static uint32_t tick_seed = 1;

#define UPDATE_LIST_RING_SIZE 8
static update_entry_t* update_list[UPDATE_LIST_RING_SIZE];

static inline void world_update_block_cache(int32_t cx, int32_t cy, int32_t cz);

uint8_t world_get_top_opaque(int32_t cx, int32_t cz)
{
	return level_opaque_height[cz][cx];
}

uint32_t world_get_vis_blocks_unsafe(int32_t cx, int32_t cy, int32_t cz)
{
	return level_has_vis_blocks[cy>>2][cz>>2][cx>>2];
}

inline int32_t world_get_render_faces_unsafe(int32_t cx, int32_t cy, int32_t cz)
{
	return level_faces[cy][cz][cx];
}

inline int32_t world_get_block_unsafe(int32_t cx, int32_t cy, int32_t cz)
{
	return fsys_level[cy][cz][cx];
}

inline int32_t world_get_block(int32_t cx, int32_t cy, int32_t cz)
{
	if(cx < 0 || cx >= LEVEL_LX) { return 0; }
	if(cy < 0 || cy >= LEVEL_LY) { return 0; }
	if(cz < 0 || cz >= LEVEL_LZ) { return 0; }
	return fsys_level[cy][cz][cx];
}

// FLAGS:
// 1 - cause block update to neighbors and self
void world_set_block(int32_t cx, int32_t cy, int32_t cz, uint8_t b, uint8_t flags)
{
	if (cx >= 0 && cy >= 0 && cz >= 0 && cx < LEVEL_LX && cy < LEVEL_LY && cz < LEVEL_LZ) {
		uint8_t old_b = fsys_level[cy][cz][cx];
		fsys_level[cy][cz][cx] = b;
		world_update_block_cache(cx, cy, cz);
		if (cy > 0) world_update_block_cache(cx, cy-1, cz);
		if (cy < LEVEL_LY-1) world_update_block_cache(cx, cy+1, cz);
		if (cx > 0) world_update_block_cache(cx-1, cy, cz);
		if (cz > 0) world_update_block_cache(cx, cy, cz-1);
		if (cx < LEVEL_LX-1) world_update_block_cache(cx+1, cy, cz);
		if (cz < LEVEL_LZ-1) world_update_block_cache(cx, cy, cz+1);
		if ((flags & 1) != 0) {
			if (old_b == 19) { // sponge
				for (int dy = -3; dy <= 3; dy++)
				for (int dz = -3; dz <= 3; dz++)
				for (int dx = -3; dx <= 3; dx++)
					if ((world_get_block(cx+dx, cy+dy, cz+dz) & (~1)) == 8) {
						world_schedule_block_update(cx+dx, cy+dy, cz+dz, 1);
					}
			}

			world_schedule_block_update(cx, cy, cz, 1);
			world_schedule_block_update(cx-1, cy, cz, 1);
			world_schedule_block_update(cx+1, cy, cz, 1);
			world_schedule_block_update(cx, cy-1, cz, 1);
			world_schedule_block_update(cx, cy+1, cz, 1);
			world_schedule_block_update(cx, cy, cz-1, 1);
			world_schedule_block_update(cx, cy, cz+1, 1);
		}
	}
}

uint32_t world_is_raycast_hit(int32_t b);

int32_t world_cast_ray(int32_t px, int32_t py, int32_t pz, int32_t vx, int32_t vy, int32_t vz, int32_t *ocx, int32_t *ocy, int32_t *ocz, int32_t max_steps, bool use_block_before_hit)
{
	// Get cell
	int32_t cx = px>>8;
	int32_t cy = py>>8;
	int32_t cz = pz>>8;

	// Get direction
	int32_t gx = (vx < 0 ? -1 : 1);
	int32_t gy = (vy < 0 ? -1 : 1);
	int32_t gz = (vz < 0 ? -1 : 1);

	// Get absolute velocity
	int32_t avx = (vx < 0 ? -vx : vx);
	int32_t avy = (vy < 0 ? -vy : vy);
	int32_t avz = (vz < 0 ? -vz : vz);

	// Avoid div by zero
	if(avx == 0) { avx = 1; }
	if(avy == 0) { avy = 1; }
	if(avz == 0) { avz = 1; }

	// Get distance to cell boundary
	int32_t dx = (vx < 0 ? (px&0xFF) : 0x100-(px&0xFF));
	int32_t dy = (vy < 0 ? (py&0xFF) : 0x100-(py&0xFF));
	int32_t dz = (vz < 0 ? (pz&0xFF) : 0x100-(pz&0xFF));

	// Get time to cell boundary
	int32_t tx = ((dx<<16)/avx);
	int32_t ty = ((dy<<16)/avy);
	int32_t tz = ((dz<<16)/avz);
	int32_t tbx = ((0x100<<16)/avx);
	int32_t tby = ((0x100<<16)/avy);
	int32_t tbz = ((0x100<<16)/avz);

	// Start tracing
	for(int32_t i = 0; i < max_steps; i++) {
		int32_t lcx = cx;
		int32_t lcy = cy;
		int32_t lcz = cz;
		int32_t face_hit = -1;

		// Find nearest boundary
		if(tx < ty && tx < tz) {
			// X
			cx += gx;
			ty -= tx;
			tz -= tx;
			tx = tbx;
			face_hit = gx < 0 ? FACE_XP : FACE_XN;
		} else if(ty < tz) {
			// Y
			tx -= ty;
			cy += gy;
			tz -= ty;
			ty = tby;
			face_hit = gy < 0 ? FACE_YP : FACE_YN;
		} else {
			// Z
			tx -= tz;
			ty -= tz;
			cz += gz;
			tz = tbz;
			face_hit = gz < 0 ? FACE_ZP : FACE_ZN;
		}

		// Check if our cell is good
		if(world_is_raycast_hit(world_get_block(cx, cy, cz))) {
			// We hit something
			if(use_block_before_hit) {
				*ocx = lcx;
				*ocy = lcy;
				*ocz = lcz;
			} else {
				*ocx = cx;
				*ocy = cy;
				*ocz = cz;
			}

			if (*ocx < 0 || *ocy < 0 || *ocz < 0 || *ocx >= LEVEL_LX || *ocy >= LEVEL_LY || *ocz >= LEVEL_LZ)
				return -1;

			// And return
			return face_hit;
		}

	}

	// We hit nothing
	return -1;
}

inline uint32_t world_is_translucent(int32_t b) {
	return b == 0 || b == 6 || b == 18 || b == 20 || (b >= 37 && b <= 40) || b == 44;
}

inline uint32_t world_is_translucent_render(int32_t b) {
	// TODO: We could probably handle lava a bit more economically?
	return world_is_translucent(b) || (b >= 8 && b <= 11);
}

inline uint32_t world_is_walkable(int32_t b) {
	return b == 0 || b == 6 || (b >= 8 && b <= 11) || (b >= 37 && b <= 40);
}

inline uint32_t world_is_raycast_hit(int32_t b) {
	return b != 0 && !(b >= 8 && b <= 11);
}

int32_t world_is_colliding(int32_t x1, int32_t y1, int32_t z1, int32_t x2, int32_t y2, int32_t z2) {
	if (x2 < 0 || y2 < 0 || z2 < 0 || x1 >= LEVEL_LX || z1 >= LEVEL_LZ)
		return true;
	if (y1 >= LEVEL_LY)
		return false;

	for (int y = y1; y <= y2; y++)
	for (int z = z1; z <= z2; z++)
	for (int x = x1; x <= x2; x++)
		if (!world_is_walkable(world_get_block(x, y, z)))
			return true;
	return false;
}

int32_t world_is_colliding_fixed(int32_t x1, int32_t y1, int32_t z1, int32_t x2, int32_t y2, int32_t z2) {
	if (x1 < 0 || y1 < 0 || z1 < 0 || x2 >= LEVEL_LX<<8 || z2 >= LEVEL_LZ<<8)
		return true;
	if (y1 >= LEVEL_LY<<8)
		return false;

	for (int y = (y1>>8); y <= (y2>>8); y++)
	for (int z = (z1>>8); z <= (z2>>8); z++)
	for (int x = (x1>>8); x <= (x2>>8); x++)
	{
		int block_id = world_get_block(x, y, z);
		if (block_id == 44)
		{
			if (y1 < ((y << 8) | 0x80))
				return true;
		}
		else if (!world_is_walkable(block_id))
			return true;
	}

	return false;
}

static inline uint32_t equal_render(int32_t b, int32_t nb) {
	if ((b >= 8) && (b <= 11)) return (b&(~1))==(nb&(~1));
	else return b == nb;
}

static inline uint32_t should_render(int32_t b, int32_t nx, int32_t ny, int32_t nz, int32_t face) {
	int32_t nb = world_get_block_unsafe(nx, ny, nz);
	switch (b) {
		case 6:
		case 37:
		case 38:
		case 39:
		case 40: // plants
			return 1;
		case 8:
		case 9: // water
		case 10:
		case 11: // lava
			return (b&(~1)) != (nb&(~1)) && world_is_translucent_render(nb);
		case 18: // leaves
		case 20: // glass
			return b != nb && world_is_translucent_render(nb);
		case 44: // single slab
			if (face == FACE_YP) return 1;
			else return world_is_translucent_render(nb);
		default: // solid blocks
			switch (nb) {
				case 44:
					if (face != FACE_YP) return 1;
					return world_is_translucent_render(nb);
				default:
					return world_is_translucent_render(nb);
			}
			break;
	}
}

static inline uint32_t calc_fmask(int32_t cx, int32_t cy, int32_t cz, int32_t b)
{
	if(b == 0) {
		return 0;
	}

	uint32_t fmask = 0;

	if(cz > 0 && should_render(b, cx, cy, cz-1, FACE_ZN)) fmask |= 0x01;
	if(cx > 0 && should_render(b, cx-1, cy, cz, FACE_XN)) fmask |= 0x04;
	if(cy > 0 && should_render(b, cx, cy-1, cz, FACE_YN)) fmask |= 0x10;
	if(cz < LEVEL_LZ-1 && should_render(b, cx, cy, cz+1, FACE_ZP)) fmask |= 0x02;
	if(cx < LEVEL_LX-1 && should_render(b, cx+1, cy, cz, FACE_XP)) fmask |= 0x08;
	if(cy >= LEVEL_LY-1 || should_render(b, cx, cy+1, cz, FACE_YP)) fmask |= 0x20;

	return fmask;
}

static inline void world_update_block_cache(int32_t cx, int32_t cy, int32_t cz) {
	int32_t b = world_get_block_unsafe(cx, cy, cz);

	// update hasblock
	/* if (b != 0) {
		level_has_block[cy][cz][cx >> 6] &= ~(1 << (cx & 0x3F));
	} else {
		level_has_block[cy][cz][cx >> 6] |= (1 << (cx & 0x3F));
	} */

	// update fmask
	level_faces[cy][cz][cx] = calc_fmask(cx, cy, cz, b);

	// update heightmap
	uint32_t translucent = world_is_translucent(b);
	if (!translucent && level_opaque_height[cz][cx] < cy) {
		level_opaque_height[cz][cx] = cy;
	} else if (translucent && level_opaque_height[cz][cx] >= cy) {
		int py = level_opaque_height[cz][cx];
		level_opaque_height[cz][cx] = 0;
		for (; py > 0; py--) {
			if (!world_is_translucent(world_get_block_unsafe(cx, py, cz))) {
				level_opaque_height[cz][cx] = py;
				break;
			}
		}
	}

	// update partial octree
	uint32_t visbit = 1;
	visbit <<= (cz&3);
	visbit <<= (cy&3)<<2;
	if(((uint32_t *)level_faces[cy][cz])[cx>>2] != 0) {
		level_has_vis_blocks[cy>>2][cz>>2][cx>>2] |= visbit;
	} else {
		level_has_vis_blocks[cy>>2][cz>>2][cx>>2] &= ~visbit;
	}
}

static bool is_liquid(int32_t b) {
	return b >= 8 && b <= 11;
}

static void world_liquid_try_expand(int32_t cx, int32_t cy, int32_t cz, int32_t dir, int32_t db, uint32_t delay, bool sponges)
{
	(void)dir;
	if (cx < 0 || cy < 0 || cz < 0 || cx >= LEVEL_LX || cy >= LEVEL_LY || cz >= LEVEL_LZ)
		return;

	int32_t b = world_get_block_unsafe(cx, cy, cz);
	if (is_liquid(b)) {
		b &= ~1;
		db &= ~1;
		if (db == 10 && b == 8) { // Lava -> Water = Stone
			world_set_block(cx, cy, cz, 1, 0);
		} else if (db == 8 && b == 10) { // Water -> Lava = Obsidian
			world_set_block(cx, cy, cz, 49, 0);
		}
	} else if ((b&(~1)) != (db&(~1)) && world_is_walkable(b)) {
		if (sponges) {
			int min_x = cx - 2;
			int min_y = cy - 2;
			int min_z = cz - 2;
			int max_x = cx + 2;
			int max_y = cy + 2;
			int max_z = cz + 2;

			if (min_x < 0) min_x = 0;
			else if (max_x >= LEVEL_LX) max_x = LEVEL_LX - 1;

			if (min_y < 0) min_y = 0;
			else if (max_y >= LEVEL_LY) max_y = LEVEL_LY - 1;

			if (min_z < 0) min_z = 0;
			else if (max_z >= LEVEL_LZ) max_z = LEVEL_LZ - 1;

			for (int dy = min_y; dy <= max_y; dy++)
			for (int dz = min_z; dz <= max_z; dz++)
			for (int dx = min_x; dx <= max_x; dx++)
				if (world_get_block_unsafe(dx, dy, dz) == 19)
					return;
		}
		world_set_block(cx, cy, cz, db, 0);
		world_schedule_block_update(cx, cy, cz, delay);
	}
}

static void world_falling_try(int32_t cx, int32_t cy, int32_t cz, int32_t db) {
#ifdef WORLD_FALLING_CLASSIC
	int dy = cy;
	while (world_is_walkable(world_get_block(cx, --dy, cz))) {}
	if (++dy != cy) {
		world_set_block(cx, cy, cz, 0, 1);
		world_set_block(cx, dy, cz, db, 1);
	}
#else
	int32_t b = world_get_block(cx, cy-1, cz);
	if (world_is_walkable(b)) {
		world_set_block(cx, cy, cz, 0, 1);
		world_set_block(cx, cy-1, cz, db, 1);
	}
#endif
}

static void world_block_check(int32_t cx, int32_t cy, int32_t cz) {
	int32_t b = world_get_block_unsafe(cx, cy, cz);
	switch (b) {
		case 2: // grass
			if (world_get_top_opaque(cx, cz) > cy)
				world_set_block(cx, cy, cz, 3, 0);
			break;
		case 3: // dirt
			if (world_get_top_opaque(cx, cz) <= cy) {
				for (int dy = cy-1; dy <= cy+1; dy++)
				for (int dz = cz-1; dz <= cz+1; dz++)
				for (int dx = cx-1; dx <= cx+1; dx++)
					if (world_get_block(dx, dy, dz) == 2) {
						// ^ also convienently checks world position validity
						if (world_get_top_opaque(dx, dz) <= dy)
							world_set_block(cx, cy, cz, 2, 0);
					}
			}
			break;
		case 6: // sapling
			break;
		case 37:
		case 38: 
			if (cy == 0 || (world_get_block_unsafe(cx, cy-1, cz)&(~1)) != 2 || world_get_top_opaque(cx, cz) > cy)
				world_set_block(cx, cy, cz, 0, 0);
			break;
		case 39:
		case 40:
			if (cy == 0 || (world_get_block_unsafe(cx, cy-1, cz)) != 1 || world_get_top_opaque(cx, cz) <= cy)
				world_set_block(cx, cy, cz, 0, 0);
			break;
	}
}

static void world_block_update(int32_t cx, int32_t cy, int32_t cz) {
	int32_t b = world_get_block_unsafe(cx, cy, cz);
	switch (b) {
		case 8: // water
		case 9:
			world_liquid_try_expand(cx, cy-1, cz, 4, 8, 4, true);
			world_liquid_try_expand(cx-1, cy, cz, 2, 8, 4, true);
			world_liquid_try_expand(cx+1, cy, cz, 3, 8, 4, true);
			world_liquid_try_expand(cx, cy, cz-1, 0, 8, 4, true);
			world_liquid_try_expand(cx, cy, cz+1, 1, 8, 4, true);
			break;
		case 10: // lava
		case 11:
			world_liquid_try_expand(cx, cy-1, cz, 4, 10, 16, false);
			world_liquid_try_expand(cx-1, cy, cz, 2, 10, 16, false);
			world_liquid_try_expand(cx+1, cy, cz, 3, 10, 16, false);
			world_liquid_try_expand(cx, cy, cz-1, 0, 10, 16, false);
			world_liquid_try_expand(cx, cy, cz+1, 1, 10, 16, false);
			break;
		case 12: // sand
		case 13: // gravel
			world_falling_try(cx, cy, cz, b);
			break;
		case 19: // sponge
			for (int dy = -2; dy <= 2; dy++)
			for (int dz = -2; dz <= 2; dz++)
			for (int dx = -2; dx <= 2; dx++)
				if ((world_get_block(cx+dx, cy+dy, cz+dz) & (~1)) == 8) {
					world_set_block(cx+dx, cy+dy, cz+dz, 0, 1);
				}
			break;
		default:
			break;
	}
}

static inline void world_tick() {
	if ((level_time & 3) == 0) {
		// TODO: consider using LFSR
		for (int i = 0; i < 64; i++)
		{
			int r = RAND(tick_seed);
			int ry = r % LEVEL_LY;
			int rz = (r / LEVEL_LY) % LEVEL_LZ;
			int rx = ((r / LEVEL_LY) / LEVEL_LZ) % LEVEL_LX;

			world_block_check(rx, ry, rz);
		}

		uint32_t update_time = (level_time >> 2);
		int ring_pos = (update_time % UPDATE_LIST_RING_SIZE);
		while (ring_pos < 0) ring_pos += UPDATE_LIST_RING_SIZE;

		update_entry_t* prev = NULL;
		update_entry_t* curr = update_list[ring_pos];
		while (curr != NULL) {
			if (update_time >= curr->time) {
				int32_t cx = curr->cx;
				int32_t cy = curr->cy;
				int32_t cz = curr->cz;

				update_entry_t* f_curr = curr;
				if (prev != NULL) {
					prev->next = curr->next;
					world_block_update(cx, cy, cz);
					curr = prev->next;
				} else {
					update_list[ring_pos] = curr->next;
					world_block_update(cx, cy, cz);
					curr = update_list[ring_pos];
				}
				free(f_curr);
			} else {
				prev = curr;
				curr = curr->next;
			}
		}
	}
}

void world_init() {
	for (int cy = 0; cy < LEVEL_LY; cy++)
	for (int cz = 0; cz < LEVEL_LZ; cz++)
	for (int cx = 0; cx < LEVEL_LX; cx++)
		world_update_block_cache(cx, cy, cz);
}

void world_update(uint32_t ticks, uint32_t *vbl_counter) {
	target_level_time = ticks;
	uint32_t in_vbls = (*vbl_counter);

	while (level_time < target_level_time) {
		world_tick();
		level_time++;

		if (((*vbl_counter) - in_vbls) >= 2) break;
	}
}

void world_schedule_block_update(int32_t cx, int32_t cy, int32_t cz, uint32_t delay) {
	if (cx < 0 || cy < 0 || cz < 0 || cx >= LEVEL_LX || cy >= LEVEL_LY || cz >= LEVEL_LZ)
		return;

	update_entry_t* entry = (update_entry_t*) malloc(sizeof(update_entry_t));
	if (entry == NULL)
		return; // ah well

	entry->cx = (uint8_t) cx;
	entry->cy = (uint8_t) cy;
	entry->cz = (uint8_t) cz;
	entry->time = ((level_time + 3) >> 2) + delay;

	int ring_pos = (entry->time % UPDATE_LIST_RING_SIZE);
	while (ring_pos < 0) ring_pos += UPDATE_LIST_RING_SIZE;

	entry->next = update_list[ring_pos];
	update_list[ring_pos] = entry;
}
