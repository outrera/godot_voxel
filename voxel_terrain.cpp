#include "voxel_terrain.h"
#include "voxel_map.h"
#include "voxel_block.h"
#include "voxel_provider_thread.h"
#include "voxel_raycast.h"
#include "voxel_provider_test.h"
#include "utility.h"

#include <core/os/os.h>
#include <scene/3d/mesh_instance.h>
#include <core/engine.h>


VoxelTerrain::VoxelTerrain()
	: Spatial(), _generate_collisions(true) {

	_map = Ref<VoxelMap>(memnew(VoxelMap));

	_view_distance_blocks = 8;
	_last_view_distance_blocks = 0;

	_provider_thread = NULL;
	_block_updater = NULL;

	_generate_collisions = false;
	_run_in_editor = false;
}

VoxelTerrain::~VoxelTerrain() {
	print_line("Destroying VoxelTerrain");
	if(_provider_thread) {
		memdelete(_provider_thread);
	}
	if(_block_updater) {
		memdelete(_block_updater);
	}
}

// TODO See if there is a way to specify materials in voxels directly?

bool VoxelTerrain::_set(const StringName &p_name, const Variant &p_value) {

	if (p_name.operator String().begins_with("material/")) {
		int idx = p_name.operator String().get_slicec('/', 1).to_int();
		ERR_FAIL_COND_V(idx >= VoxelMesher::MAX_MATERIALS || idx < 0, false);
		set_material(idx, p_value);
		return true;
	}

	return false;
}

bool VoxelTerrain::_get(const StringName &p_name, Variant &r_ret) const {

	if (p_name.operator String().begins_with("material/")) {
		int idx = p_name.operator String().get_slicec('/', 1).to_int();
		ERR_FAIL_COND_V(idx >= VoxelMesher::MAX_MATERIALS || idx < 0, false);
		r_ret = get_material(idx);
		return true;
	}

	return false;
}

void VoxelTerrain::_get_property_list(List<PropertyInfo> *p_list) const {

	for (int i = 0; i < VoxelMesher::MAX_MATERIALS; ++i) {
		p_list->push_back(PropertyInfo(Variant::OBJECT, "material/" + itos(i), PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial,SpatialMaterial"));
	}
}

void VoxelTerrain::set_provider(Ref<VoxelProvider> provider) {
	if(provider != _provider) {

		if(_provider_thread) {
			memdelete(_provider_thread);
			_provider_thread = NULL;
		}

		_provider = provider;
		_provider_thread = memnew(VoxelProviderThread(_provider, _map->get_block_size_pow2()));
//		Ref<VoxelProviderTest> test;
//		test.instance();
//		_provider_thread = memnew(VoxelProviderThread(test, _map->get_block_size_pow2()));

		// The whole map might change, so make all area dirty
		// TODO Actually, we should regenerate the whole map, not just update all its blocks
		make_all_view_dirty_deferred();
	}
}

Ref<VoxelProvider> VoxelTerrain::get_provider() const {
	return _provider;
}

Ref<VoxelLibrary> VoxelTerrain::get_voxel_library() const {
	return _library;
}

void VoxelTerrain::set_voxel_library(Ref<VoxelLibrary> library) {

	if (library != _library) {

#ifdef TOOLS_ENABLED
		if (library->get_voxel_count() == 0) {
			library->load_default();
		}
#endif
		_library = library;

		if(_block_updater) {
			memdelete(_block_updater);
			_block_updater = NULL;
		}

		// TODO Thread-safe way to change those parameters
		VoxelMeshUpdater::MeshingParams params;

		_block_updater = memnew(VoxelMeshUpdater(_library, params));

		// Voxel appearance might completely change
		make_all_view_dirty_deferred();
	}
}

void VoxelTerrain::set_generate_collisions(bool enabled) {
	_generate_collisions = enabled;
}

int VoxelTerrain::get_view_distance() const {
	return _view_distance_blocks * _map->get_block_size();
}

void VoxelTerrain::set_view_distance(int distance_in_voxels) {
	ERR_FAIL_COND(distance_in_voxels < 0)
	int d = distance_in_voxels / _map->get_block_size();
	if(d != _view_distance_blocks) {
		print_line(String("View distance changed from ") + String::num(_view_distance_blocks) + String(" blocks to ") + String::num(d));
		_view_distance_blocks = d;
		// Blocks too far away will be removed in _process, same for blocks to load
	}
}

void VoxelTerrain::set_viewer_path(NodePath path) {
	_viewer_path = path;
}

NodePath VoxelTerrain::get_viewer_path() const {
	return _viewer_path;
}

Spatial *VoxelTerrain::get_viewer(NodePath path) const {
	if (path.is_empty())
		return NULL;
	Node *node = get_node(path);
	if (node == NULL)
		return NULL;
	return Object::cast_to<Spatial>(node);
}

void VoxelTerrain::set_material(int id, Ref<Material> material) {
	// TODO Update existing block surfaces
	ERR_FAIL_COND(id < 0 || id >= VoxelMesher::MAX_MATERIALS);
	_materials[id] = material;
}

Ref<Material> VoxelTerrain::get_material(int id) const {
	ERR_FAIL_COND_V(id < 0 || id >= VoxelMesher::MAX_MATERIALS, Ref<Material>());
	return _materials[id];
}

void VoxelTerrain::make_block_dirty(Vector3i bpos) {
	// TODO Immediate update viewer distance?

	VoxelTerrain::BlockDirtyState *state = _dirty_blocks.getptr(bpos);

	if(state == NULL) {
		// The block is not dirty, so it will either be loaded or updated

		if(_map->has_block(bpos)) {

			_blocks_pending_update.push_back(bpos);
			_dirty_blocks[bpos] = BLOCK_UPDATE_NOT_SENT;

		} else {
			_blocks_pending_load.push_back(bpos);
			_dirty_blocks[bpos] = BLOCK_LOAD;
		}

	} else if(*state == BLOCK_UPDATE_SENT) {
		// The updater is already processing the block,
		// but the block was modified again so we schedule another update
		*state = BLOCK_UPDATE_NOT_SENT;
		_blocks_pending_update.push_back(bpos);
	}

	//OS::get_singleton()->print("Dirty (%i, %i, %i)", bpos.x, bpos.y, bpos.z);

	// TODO What if a block is made dirty, goes through threaded update, then gets changed again before it gets updated?
	// this will make the second change ignored, which is not correct!
}

void VoxelTerrain::immerge_block(Vector3i bpos) {

	ERR_FAIL_COND(_map.is_null());

	// TODO Schedule block saving when supported
	_map->remove_block(bpos, VoxelMap::NoAction());

	_dirty_blocks.erase(bpos);
	// Blocks in the update queue will be cancelled in _process,
	// because it's too expensive to linear-search all blocks for each block
}

Dictionary VoxelTerrain::get_statistics() const {

	Dictionary provider;
	provider["min_time"] = _stats.provider.min_time;
	provider["max_time"] = _stats.provider.max_time;
	provider["remaining_blocks"] = _stats.provider.remaining_blocks;
	provider["dropped_blocks"] = _stats.dropped_provider_blocks;

	Dictionary updater;
	updater["min_time"] = _stats.updater.min_time;
	updater["max_time"] = _stats.updater.max_time;
	updater["remaining_blocks"] = _stats.updater.remaining_blocks;
	updater["updated_blocks"] = _stats.updated_blocks;
	updater["mesh_alloc_time"] = _stats.mesh_alloc_time;
	updater["dropped_blocks"] = _stats.dropped_updater_blocks;
	updater["remaining_main_thread_blocks"] = _stats.remaining_main_thread_blocks;

	Dictionary d;
	d["provider"] = provider;
	d["updater"] = updater;

	// Breakdown of time spent in _process
	d["time_detect_required_blocks"] = _stats.time_detect_required_blocks;
	d["time_send_load_requests"] = _stats.time_send_load_requests;
	d["time_process_load_responses"] = _stats.time_process_load_responses;
	d["time_send_update_requests"] = _stats.time_send_update_requests;
	d["time_process_update_responses"] = _stats.time_process_update_responses;

	return d;
}

bool VoxelTerrain::is_block_dirty(Vector3i bpos) const {
	return _dirty_blocks.has(bpos);
}

//void VoxelTerrain::make_blocks_dirty(Vector3i min, Vector3i size) {
//	Vector3i max = min + size;
//	Vector3i pos;
//	for (pos.z = min.z; pos.z < max.z; ++pos.z) {
//		for (pos.y = min.y; pos.y < max.y; ++pos.y) {
//			for (pos.x = min.x; pos.x < max.x; ++pos.x) {
//				make_block_dirty(pos);
//			}
//		}
//	}
//}

void VoxelTerrain::make_all_view_dirty_deferred() {
	// This trick will regenerate all chunks in view, according to the view distance found during block updates.
	// The point of doing this instead of immediately scheduling updates is that it will
	// always use an up-to-date view distance, which is not necessarily loaded yet on initialization.
	_last_view_distance_blocks = 0;

//	Vector3i radius(_view_distance_blocks, _view_distance_blocks, _view_distance_blocks);
//	make_blocks_dirty(-radius, 2*radius);
}

inline int get_border_index(int x, int max) {
	return x == 0 ? 0 : x != max ? 1 : 2;
}

void VoxelTerrain::make_voxel_dirty(Vector3i pos) {

	// Update the block in which the voxel is
	Vector3i bpos = _map->voxel_to_block(pos);
	make_block_dirty(bpos);
	//OS::get_singleton()->print("Dirty (%i, %i, %i)\n", bpos.x, bpos.y, bpos.z);

	// Update neighbor blocks if the voxel is touching a boundary

	Vector3i rpos = _map->to_local(pos);

	// TODO Thread-safe way of getting this parameter
	bool check_corners = true;//_mesher->get_occlusion_enabled();

	const int max = _map->get_block_size() - 1;

	if (rpos.x == 0)
		make_block_dirty(bpos - Vector3i(1, 0, 0));
	else if (rpos.x == max)
		make_block_dirty(bpos + Vector3i(1, 0, 0));

	if (rpos.y == 0)
		make_block_dirty(bpos - Vector3i(0, 1, 0));
	else if (rpos.y == max)
		make_block_dirty(bpos + Vector3i(0, 1, 0));

	if (rpos.z == 0)
		make_block_dirty(bpos - Vector3i(0, 0, 1));
	else if (rpos.z == max)
		make_block_dirty(bpos + Vector3i(0, 0, 1));

	// We might want to update blocks in corners in order to update ambient occlusion
	if (check_corners) {

		//       24------25------26
		//       /|              /|
		//      / |             / |
		//    21  |           23  |
		//    /  15           /  17
		//   /    |          /    |
		// 18------19------20     |
		//  |     |         |     |
		//  |     6-------7-|-----8
		//  |    /          |    /
		//  9   /          11   /
		//  |  3            |  5
		//  | /             | /      y z
		//  |/              |/       |/
		//  0-------1-------2        o--x

		// I'm not good at writing piles of ifs

		static const int normals[27][3] = {
			{ -1, -1, -1 }, { 0, -1, -1 }, { 1, -1, -1 },
			{ -1, -1, 0 }, { 0, -1, 0 }, { 1, -1, 0 },
			{ -1, -1, 1 }, { 0, -1, 1 }, { 1, -1, 1 },

			{ -1, 0, -1 }, { 0, 0, -1 }, { 1, 0, -1 },
			{ -1, 0, 0 }, { 0, 0, 0 }, { 1, 0, 0 },
			{ -1, 0, 1 }, { 0, 0, 1 }, { 1, 0, 1 },

			{ -1, 1, -1 }, { 0, 1, -1 }, { 1, 1, -1 },
			{ -1, 1, 0 }, { 0, 1, 0 }, { 1, 1, 0 },
			{ -1, 1, 1 }, { 0, 1, 1 }, { 1, 1, 1 }
		};
		static const int ce_counts[27] = {
			4, 1, 4,
			1, 0, 1,
			4, 1, 4,

			1, 0, 1,
			0, 0, 0,
			1, 0, 1,

			4, 1, 4,
			1, 0, 1,
			4, 1, 4
		};
		static const int ce_indexes_lut[27][4] = {
			{ 0, 1, 3, 9 }, { 1 }, { 2, 1, 5, 11 },
			{ 3 }, {}, { 5 },
			{ 6, 3, 7, 15 }, { 7 }, { 8, 7, 5, 17 },

			{ 9 }, {}, { 11 },
			{}, {}, {},
			{ 15 }, {}, { 17 },

			{ 18, 9, 19, 21 }, { 19 }, { 20, 11, 19, 23 },
			{ 21 }, {}, { 23 },
			{ 24, 15, 21, 25 }, { 25 }, { 26, 17, 23, 25 }
		};

		int m = get_border_index(rpos.x, max) + 3 * get_border_index(rpos.z, max) + 9 * get_border_index(rpos.y, max);

		const int *ce_indexes = ce_indexes_lut[m];
		int ce_count = ce_counts[m];
		//OS::get_singleton()->print("m=%i, rpos=(%i, %i, %i)\n", m, rpos.x, rpos.y, rpos.z);

		for (int i = 0; i < ce_count; ++i) {
			// TODO Because it's about ambient occlusion across 1 voxel only,
			// we could optimize it even more by looking at neighbor voxels,
			// and discard the update if we know it won't change anything
			const int *normal = normals[ce_indexes[i]];
			Vector3i nbpos(bpos.x + normal[0], bpos.y + normal[1], bpos.z + normal[2]);
			//OS::get_singleton()->print("Corner dirty (%i, %i, %i)\n", nbpos.x, nbpos.y, nbpos.z);
			make_block_dirty(nbpos);
		}
	}
}

void VoxelTerrain::make_area_dirty(Rect3i box) {

	Vector3i min_pos = box.pos;
	Vector3i max_pos = box.pos + box.size - Vector3(1, 1, 1);

	// TODO Thread-safe way of getting this parameter
	bool check_corners = true;//_mesher->get_occlusion_enabled();
	if (check_corners) {

		min_pos -= Vector3i(1, 1, 1);
		max_pos += Vector3i(1, 1, 1);

	} else {

		Vector3i min_rpos = _map->to_local(min_pos);
		if (min_rpos.x == 0)
			--min_pos.x;
		if (min_rpos.y == 0)
			--min_pos.y;
		if (min_rpos.z == 0)
			--min_pos.z;

		const int max = _map->get_block_size() - 1;
		Vector3i max_rpos = _map->to_local(max_pos);
		if (max_rpos.x == max)
			++max_pos.x;
		if (max_rpos.y == max)
			++max_pos.y;
		if (max_rpos.z == max)
			++max_pos.z;
	}

	Vector3i min_block_pos = _map->voxel_to_block(min_pos);
	Vector3i max_block_pos = _map->voxel_to_block(max_pos);

	Vector3i bpos;
	for (bpos.z = min_block_pos.z; bpos.z <= max_block_pos.z; ++bpos.z) {
		for (bpos.x = min_block_pos.x; bpos.x <= max_block_pos.x; ++bpos.x) {
			for (bpos.y = min_block_pos.y; bpos.y <= max_block_pos.y; ++bpos.y) {

				make_block_dirty(bpos);
			}
		}
	}
}

struct EnterWorldAction {
	World *world;
	EnterWorldAction(World *w) : world(w) {}
	void operator()(VoxelBlock *block) {
		block->enter_world(world);
	}
};

struct ExitWorldAction {
	void operator()(VoxelBlock *block) {
		block->exit_world();
	}
};

struct SetVisibilityAction {
	bool visible;
	SetVisibilityAction(bool v) : visible(v) {}
	void operator()(VoxelBlock *block) {
		block->set_visible(visible);
	}
};

void VoxelTerrain::_notification(int p_what) {

	switch (p_what) {

		case NOTIFICATION_ENTER_TREE:
			set_process(true);
			break;

		case NOTIFICATION_PROCESS:
			if (!Engine::get_singleton()->is_editor_hint() || _run_in_editor)
				_process();
			break;

		case NOTIFICATION_EXIT_TREE:
			break;

		case NOTIFICATION_ENTER_WORLD: {
			ERR_FAIL_COND(_map.is_null());
			_map->for_all_blocks(EnterWorldAction(*get_world()));
		} break;

		case NOTIFICATION_EXIT_WORLD:
			ERR_FAIL_COND(_map.is_null());
			_map->for_all_blocks(ExitWorldAction());
			break;

		case NOTIFICATION_VISIBILITY_CHANGED:
			ERR_FAIL_COND(_map.is_null());
			_map->for_all_blocks(SetVisibilityAction(is_visible()));
			break;

		// TODO Listen for transform changes

		default:
			break;
	}
}

void VoxelTerrain::remove_positions_outside_box(Vector<Vector3i> &positions, Rect3i box, HashMap<Vector3i, VoxelTerrain::BlockDirtyState, Vector3iHasher> &state_map) {
	for(int i = 0; i < positions.size(); ++i) {
		const Vector3i bpos = positions[i];
		if(!box.contains(bpos)) {
			int last = positions.size() - 1;
			positions.write[i] = positions[last];
			positions.resize(last);
			state_map.erase(bpos);
			--i;
		}
	}
}

static inline bool is_mesh_empty(Ref<Mesh> mesh_ref) {
	if (mesh_ref.is_null())
		return true;
	const Mesh &mesh = **mesh_ref;
	if (mesh.get_surface_count() == 0)
		return true;
	if (mesh.surface_get_array_len(0) == 0)
		return true;
	return false;
}

void VoxelTerrain::_process() {

	OS &os = *OS::get_singleton();
	Engine &engine = *Engine::get_singleton();

	ERR_FAIL_COND(_map.is_null());

	uint64_t time_before = os.get_ticks_usec();

	// Get viewer location
	// TODO Transform to local (Spatial Transform)
	Vector3i viewer_block_pos;
	if(engine.is_editor_hint()) {
		// TODO Use editor's camera here
		viewer_block_pos = Vector3i();
	} else {
		Spatial *viewer = get_viewer(_viewer_path);
		if (viewer)
			viewer_block_pos = _map->voxel_to_block(viewer->get_translation());
		else
			viewer_block_pos = Vector3i();
	}

	// Find out which blocks need to appear and which need to be unloaded
	{
		//Vector3i viewer_block_pos_delta = _last_viewer_block_pos - viewer_block_pos;
		Rect3i new_box = Rect3i::from_center_extents(viewer_block_pos, Vector3i(_view_distance_blocks));
		Rect3i prev_box = Rect3i::from_center_extents(_last_viewer_block_pos, Vector3i(_last_view_distance_blocks));

		if(prev_box != new_box) {
			//print_line(String("Loaded area changed: from ") + prev_box.to_string() + String(" to ") + new_box.to_string());

			Rect3i bounds = Rect3i::get_bounding_box(prev_box, new_box);
			Vector3i max = bounds.pos + bounds.size;

			// TODO There should be a way to only iterate relevant blocks
			Vector3i pos;
			for(pos.z = bounds.pos.z; pos.z < max.z; ++pos.z) {
				for(pos.y = bounds.pos.y; pos.y < max.y; ++pos.y) {
					for(pos.x = bounds.pos.x; pos.x < max.x; ++pos.x) {

						bool prev_contains = prev_box.contains(pos);
						bool new_contains = new_box.contains(pos);

						if(prev_contains && !new_contains) {
							// Unload block
							immerge_block(pos);

						} else if(!prev_contains && new_contains) {
							// Load or update block
							make_block_dirty(pos);
						}
					}
				}
			}
		}

		// Eliminate pending blocks that aren't needed
		remove_positions_outside_box(_blocks_pending_load, new_box, _dirty_blocks);
		remove_positions_outside_box(_blocks_pending_update, new_box, _dirty_blocks);
	}

	_stats.time_detect_required_blocks = os.get_ticks_usec() - time_before;

	_last_view_distance_blocks = _view_distance_blocks;
	_last_viewer_block_pos = viewer_block_pos;

	time_before = os.get_ticks_usec();

	// Send block loading requests
	{
		VoxelProviderThread::InputData input;

		input.priority_block_position = viewer_block_pos;
		input.blocks_to_emerge.append_array(_blocks_pending_load);
		//input.blocks_to_immerge.append_array();

		//print_line(String("Sending {0} block requests").format(varray(input.blocks_to_emerge.size())));
		_blocks_pending_load.clear();

		_provider_thread->push(input);
	}

	_stats.time_send_load_requests = os.get_ticks_usec() - time_before;
	time_before = os.get_ticks_usec();

	// Get block loading responses
	// Note: if block loading is too fast, this can cause stutters. It should only happen on first load, though.
	{
		const unsigned int bs = _map->get_block_size();
		const Vector3i block_size(bs, bs, bs);

		VoxelProviderThread::OutputData output;
		_provider_thread->pop(output);
		//print_line(String("Receiving {0} blocks").format(varray(output.emerged_blocks.size())));

		_stats.provider = output.stats;
		_stats.dropped_provider_blocks = 0;

		for(int i = 0; i < output.emerged_blocks.size(); ++i) {

			const VoxelProviderThread::EmergeOutput &o = output.emerged_blocks[i];
			Vector3i block_pos = _map->voxel_to_block(o.origin_in_voxels);

			{
				VoxelTerrain::BlockDirtyState *state = _dirty_blocks.getptr(block_pos);
				if(state == NULL || *state != BLOCK_LOAD) {
					// That block was not requested, drop it
					++_stats.dropped_provider_blocks;
					continue;
				}
			}

			// Check return
			// TODO Shouldn't halt execution though, as it can bring the map in an invalid state!
			ERR_FAIL_COND(o.voxels->get_size() != block_size);

			// TODO Discard blocks out of range

			// Store buffer
			bool update_neighbors = !_map->has_block(block_pos);
			_map->set_block_buffer(block_pos, o.voxels);

			// Trigger mesh updates
			if (update_neighbors) {
				// All neighbors have to be checked. If they are now surrounded, they can be updated
				Vector3i ndir;
				for (ndir.z = -1; ndir.z < 2; ++ndir.z) {
					for (ndir.x = -1; ndir.x < 2; ++ndir.x) {
						for (ndir.y = -1; ndir.y < 2; ++ndir.y) {
							Vector3i npos = block_pos + ndir;
							// TODO What if the map is really composed of empty blocks?
							if (_map->is_block_surrounded(npos)) {

								VoxelTerrain::BlockDirtyState *state = _dirty_blocks.getptr(npos);
								if (state && *state == BLOCK_UPDATE_NOT_SENT) {
									// Assuming it is scheduled to be updated already.
									// In case of BLOCK_UPDATE_SENT, we'll have to resend it.
									continue;
								}

								_dirty_blocks[npos] = BLOCK_UPDATE_NOT_SENT;
								_blocks_pending_update.push_back(npos);
							}
						}
					}
				}

			} else {
				// Only update the block, neighbors will probably follow if needed
				_dirty_blocks[block_pos] = BLOCK_UPDATE_NOT_SENT;
				_blocks_pending_update.push_back(block_pos);
				//OS::get_singleton()->print("Update (%i, %i, %i)\n", block_pos.x, block_pos.y, block_pos.z);
			}
		}
	}

	_stats.time_process_load_responses = os.get_ticks_usec() - time_before;
	time_before = os.get_ticks_usec();

	// Send mesh updates
	{
		VoxelMeshUpdater::Input input;

		for(int i = 0; i < _blocks_pending_update.size(); ++i) {
			Vector3i block_pos = _blocks_pending_update[i];

			VoxelBlock *block = _map->get_block(block_pos);
			if (block == NULL) {
				continue;
			}

			CRASH_COND(block->voxels.is_null());

			VoxelTerrain::BlockDirtyState *block_state = _dirty_blocks.getptr(block_pos);
			CRASH_COND(block_state == NULL);
			CRASH_COND(*block_state != BLOCK_UPDATE_NOT_SENT);

			int air_type = 0;
			if(block->voxels->is_uniform(Voxel::CHANNEL_TYPE) && block->voxels->get_voxel(0, 0, 0, Voxel::CHANNEL_TYPE) == air_type) {

				// The block contains empty voxels
				block->set_mesh(Ref<Mesh>(), Ref<World>());
				_dirty_blocks.erase(block_pos);

				// Optional, but I guess it might spare some memory
				block->voxels->clear_channel(Voxel::CHANNEL_TYPE, air_type);

				continue;
			}

			// Create buffer padded with neighbor voxels
			Ref<VoxelBuffer> nbuffer;
			nbuffer.instance();
			// TODO Make the buffer re-usable
			// TODO Padding set to 3 at the moment because Transvoxel works on 2x2 cells.
			// It should change for a smarter padding (if smooth isn't used for example).
			unsigned int block_size = _map->get_block_size();
			nbuffer->create(block_size + 3, block_size + 3, block_size + 3);

			_map->get_buffer_copy(_map->block_to_voxel(block_pos) - Vector3i(1, 1, 1), **nbuffer, 0x3);

			VoxelMeshUpdater::InputBlock iblock;
			iblock.voxels = nbuffer;
			iblock.position = block_pos;
			input.blocks.push_back(iblock);

			*block_state = BLOCK_UPDATE_SENT;
		}

		_block_updater->push(input);
		_blocks_pending_update.clear();
	}

	_stats.time_send_update_requests = os.get_ticks_usec() - time_before;
	time_before = os.get_ticks_usec();

	// Get mesh updates
	{
		{
			VoxelMeshUpdater::Output output;
			_block_updater->pop(output);

			_stats.updater = output.stats;
			_stats.updated_blocks = output.blocks.size();
			_stats.dropped_updater_blocks = 0;

			_blocks_pending_main_thread_update.append_array(output.blocks);
		}

		Ref<World> world = get_world();
		uint32_t time_before = os.get_ticks_msec();
		uint32_t timeout = os.get_ticks_msec() + 10;
		int queue_index = 0;

		// The following is done on the main thread because Godot doesn't really support multithreaded Mesh allocation.
		// This also proved to be very slow compared to the meshing process itself...
		// hopefully Vulkan will allow us to upload graphical resources without stalling rendering as they upload?

		for (; queue_index < _blocks_pending_main_thread_update.size() && os.get_ticks_msec() < timeout; ++queue_index) {

			const VoxelMeshUpdater::OutputBlock &ob = _blocks_pending_main_thread_update[queue_index];

			VoxelTerrain::BlockDirtyState *state = _dirty_blocks.getptr(ob.position);
			if (state && *state == BLOCK_UPDATE_SENT) {
				_dirty_blocks.erase(ob.position);
			}

			VoxelBlock *block = _map->get_block(ob.position);
			if (block == NULL) {
				// That block is no longer loaded, drop the result
				++_stats.dropped_updater_blocks;
				continue;
			}

			Ref<ArrayMesh> mesh;
			mesh.instance();

			int surface_index = 0;
			for (int i = 0; i < ob.model_surfaces.size(); ++i) {

				Array surface = ob.model_surfaces[i];
				if (surface.empty())
					continue;

				mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, surface);
				mesh->surface_set_material(surface_index, _materials[i]);

				++surface_index;
			}

			for(int i = 0; i < ob.smooth_surfaces.size(); ++i) {

				Array surface = ob.smooth_surfaces[i];
				if (surface.empty())
					continue;

				mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, surface);
				// No material supported yet
				++surface_index;
			}

			if (is_mesh_empty(mesh))
				mesh = Ref<Mesh>();

			block->set_mesh(mesh, world);
		}

		shift_up(_blocks_pending_main_thread_update, queue_index);

		uint32_t time_taken = os.get_ticks_msec() - time_before;
		_stats.mesh_alloc_time = time_taken;
	}

	_stats.time_process_update_responses = os.get_ticks_usec() - time_before;

	//print_line(String("d:") + String::num(_dirty_blocks.size()) + String(", q:") + String::num(_block_update_queue.size()));
}

//void VoxelTerrain::block_removed(VoxelBlock & block) {
//    MeshInstance * mesh_instance = block.get_mesh_instance(*this);
//    if (mesh_instance) {
//        mesh_instance->queue_delete();
//    }
//}

struct _VoxelTerrainRaycastContext {
	VoxelTerrain &terrain;
	//unsigned int channel_mask;
};

static bool _raycast_binding_predicate(Vector3i pos, void *context_ptr) {

	ERR_FAIL_COND_V(context_ptr == NULL, false);
	_VoxelTerrainRaycastContext *context = (_VoxelTerrainRaycastContext *)context_ptr;
	VoxelTerrain &terrain = context->terrain;

	//unsigned int channel = context->channel;

	Ref<VoxelMap> map = terrain.get_map();
	int v0 = map->get_voxel(pos, Voxel::CHANNEL_TYPE);

	Ref<VoxelLibrary> lib_ref = terrain.get_voxel_library();
	if (lib_ref.is_null())
		return false;
	const VoxelLibrary &lib = **lib_ref;

	if (lib.has_voxel(v0) == false)
		return false;

	const Voxel &voxel = lib.get_voxel_const(v0);
	if (voxel.is_transparent() == false)
		return true;

	int v1 = map->get_voxel(pos, Voxel::CHANNEL_ISOLEVEL);
	return v1 - 128 >= 0;
}

void VoxelTerrain::_make_area_dirty_binding(AABB aabb) {
	make_area_dirty(Rect3i(aabb.position, aabb.size));
}

Variant VoxelTerrain::_raycast_binding(Vector3 origin, Vector3 direction, real_t max_distance) {

	// TODO Transform input if the terrain is rotated (in the future it can be made a Spatial node)

	Vector3i hit_pos;
	Vector3i prev_pos;

	_VoxelTerrainRaycastContext context = { *this };

	if (voxel_raycast(origin, direction, _raycast_binding_predicate, &context, max_distance, hit_pos, prev_pos)) {

		Dictionary hit = Dictionary();
		hit["position"] = hit_pos.to_vec3();
		hit["prev_position"] = prev_pos.to_vec3();
		return hit;
	} else {
		return Variant(); // Null dictionary, no alloc
	}
}

Vector3 VoxelTerrain::_voxel_to_block_binding(Vector3 pos) {
	return Vector3i(_map->voxel_to_block(pos)).to_vec3();
}

Vector3 VoxelTerrain::_block_to_voxel_binding(Vector3 pos) {
	return Vector3i(_map->block_to_voxel(pos)).to_vec3();
}

// For debugging purpose
VoxelTerrain::BlockDirtyState VoxelTerrain::get_block_state(Vector3 p_bpos) const {
	Vector3i bpos = p_bpos;
	const VoxelTerrain::BlockDirtyState *state = _dirty_blocks.getptr(bpos);
	if(state) {
		return *state;
	} else {
		if(!_map->has_block(bpos))
			return BLOCK_NONE;
		return BLOCK_IDLE;
	}
}

void VoxelTerrain::_bind_methods() {

	ClassDB::bind_method(D_METHOD("set_provider", "provider"), &VoxelTerrain::set_provider);
	ClassDB::bind_method(D_METHOD("get_provider"), &VoxelTerrain::get_provider);

	ClassDB::bind_method(D_METHOD("set_voxel_library", "library"), &VoxelTerrain::set_voxel_library);
	ClassDB::bind_method(D_METHOD("get_voxel_library"), &VoxelTerrain::get_voxel_library);

	ClassDB::bind_method(D_METHOD("set_view_distance", "distance_in_voxels"), &VoxelTerrain::set_view_distance);
	ClassDB::bind_method(D_METHOD("get_view_distance"), &VoxelTerrain::get_view_distance);

	ClassDB::bind_method(D_METHOD("get_generate_collisions"), &VoxelTerrain::get_generate_collisions);
	ClassDB::bind_method(D_METHOD("set_generate_collisions", "enabled"), &VoxelTerrain::set_generate_collisions);

	ClassDB::bind_method(D_METHOD("get_viewer_path"), &VoxelTerrain::get_viewer_path);
	ClassDB::bind_method(D_METHOD("set_viewer_path", "path"), &VoxelTerrain::set_viewer_path);

	ClassDB::bind_method(D_METHOD("get_storage"), &VoxelTerrain::get_map);

	ClassDB::bind_method(D_METHOD("voxel_to_block", "voxel_pos"), &VoxelTerrain::_voxel_to_block_binding);
	ClassDB::bind_method(D_METHOD("block_to_voxel", "block_pos"), &VoxelTerrain::_block_to_voxel_binding);

	ClassDB::bind_method(D_METHOD("make_voxel_dirty", "pos"), &VoxelTerrain::_make_voxel_dirty_binding);
	ClassDB::bind_method(D_METHOD("make_area_dirty", "aabb"), &VoxelTerrain::_make_area_dirty_binding);

	ClassDB::bind_method(D_METHOD("raycast", "origin", "direction", "max_distance"), &VoxelTerrain::_raycast_binding, DEFVAL(100));

	ClassDB::bind_method(D_METHOD("get_statistics"), &VoxelTerrain::get_statistics);
	ClassDB::bind_method(D_METHOD("get_block_state", "block_pos"), &VoxelTerrain::get_block_state);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "provider", PROPERTY_HINT_RESOURCE_TYPE, "VoxelProvider"), "set_provider", "get_provider");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "voxel_library", PROPERTY_HINT_RESOURCE_TYPE, "VoxelLibrary"), "set_voxel_library", "get_voxel_library");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "view_distance"), "set_view_distance", "get_view_distance");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "viewer_path"), "set_viewer_path", "get_viewer_path");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "generate_collisions"), "set_generate_collisions", "get_generate_collisions");

	BIND_ENUM_CONSTANT(BLOCK_NONE);
	BIND_ENUM_CONSTANT(BLOCK_LOAD);
	BIND_ENUM_CONSTANT(BLOCK_UPDATE_NOT_SENT);
	BIND_ENUM_CONSTANT(BLOCK_UPDATE_SENT);
	BIND_ENUM_CONSTANT(BLOCK_IDLE);
}
