module;
#include <rstd/macro.hpp>



module wescene.parse;
import wescene.spec_texs;
import wescene.core;
import wescene.types;
import rstd.log;
import rstd.cppstd;
import wescene.scene;
import wescene.common;

using namespace owe;

namespace
{

WPPuppet::PlayMode ToPlayMode(std::string_view m) {
    if (m == "loop" || m.empty()) return WPPuppet::PlayMode::Loop;
    if (m == "mirror") return WPPuppet::PlayMode::Mirror;
    if (m == "single") return WPPuppet::PlayMode::Single;

    rstd_error("unknown puppet animation play mode \"{}\"", m);
    rstd_assert(m == "loop");
    return WPPuppet::PlayMode::Loop;
}

// Vertex layout bits (mirror imhex/mdl.hexpat MdlFlagBit).
constexpr uint32_t MDL_FLAG_NORMAL      = 0x00000002;
constexpr uint32_t MDL_FLAG_TANGENT     = 0x00000004;
constexpr uint32_t MDL_FLAG_UV          = 0x00000008;
constexpr uint32_t MDL_FLAG_UV2         = 0x00000020;
constexpr uint32_t MDL_FLAG_EXTRA4      = 0x00010000;
constexpr uint32_t MDL_FLAG_SKIN_BLEND  = 0x00800000;
constexpr uint32_t MDL_FLAG_SKIN_WEIGHT = 0x01000000;

constexpr uint32_t singile_indices    = 2 * 3;
constexpr uint32_t singile_bone_frame = 4 * 9;

// Compute per-vertex byte stride from a layout flag bitset. Position is
// always emitted (12 bytes), other attributes are gated by their bits.
uint32_t compute_vertex_stride(uint32_t flag) {
    uint32_t s = 12;
    if (flag & MDL_FLAG_NORMAL)      s += 12;
    if (flag & MDL_FLAG_TANGENT)     s += 16;
    if (flag & MDL_FLAG_EXTRA4)      s += 4;
    if (flag & MDL_FLAG_SKIN_BLEND)  s += 16;
    if (flag & MDL_FLAG_SKIN_WEIGHT) s += 16;
    if (flag & MDL_FLAG_UV)          s += 8;
    if (flag & MDL_FLAG_UV2)         s += 8;
    return s;
}

// Peek the next 4 bytes; restore cursor before returning. Used to detect
// optional MDLS/MDAT/MDLA/MDMP/MDLE block headers without consuming them.
bool peek_block_magic(fs::MemBinaryStream& f, std::string_view expect4) {
    if (expect4.size() != 4) return false;
    auto save = f.Tell();
    if (save + 4 > f.Size()) return false;
    char buf[4] = { 0 };
    f.Read(buf, 4);
    bool ok = (std::memcmp(buf, expect4.data(), 4) == 0);
    f.SeekSet(save);
    return ok;
}

void ParseMasks(fs::MemBinaryStream& f, WPMdl::Mesh& mesh);

// hexpat Mesh<MdlV, TopFlag, SinglePuppet>:
//   CStr mat_json + u32 flag_a + (if flag_a==2: u32) + (if MdlV>=17: aabb)
//   + (if MdlV>14: u32 mesh_flag) + u32 vertex_size + Vertex[]
//   + u32 indices_size + Triangle[] + (if MdlV>=21: Parts) + (if MdlV>21: Masks)
bool ParseMesh(fs::MemBinaryStream& f, const WPMdlHeader& header,
               WPMdl::Mesh& mesh, std::string_view path) {
    mesh.mat_json_file = f.ReadStr();
    mesh.flag_a        = f.ReadUint32();
    if (mesh.flag_a == 2) {
        mesh.has_flag_a2_one = (f.ReadUint32() == 1);
    }

    if (header.mdlv >= 17) {
        for (auto& v : mesh.aabb_min) v = f.ReadFloat();
        for (auto& v : mesh.aabb_max) v = f.ReadFloat();
        mesh.has_aabb = true;
    }

    uint32_t mesh_flag = (header.mdlv > 14) ? f.ReadUint32() : header.mdl_flag;
    mesh.flag          = mesh_flag;

    uint32_t vertex_size = f.ReadUint32();
    uint32_t stride      = compute_vertex_stride(mesh_flag);
    if (stride == 0 || vertex_size % stride != 0) {
        rstd_error("unsupport mdl vertex size {} (flag=0x{:X} stride={}) in {}",
                   vertex_size, mesh_flag, stride, std::string(path));
        return false;
    }

    uint32_t vertex_num = vertex_size / stride;
    mesh.positions.resize(vertex_num);
    if (mesh_flag & MDL_FLAG_NORMAL)      mesh.normals.resize(vertex_num);
    if (mesh_flag & MDL_FLAG_TANGENT)     mesh.tangents.resize(vertex_num);
    if (mesh_flag & MDL_FLAG_EXTRA4)      mesh.extra4.resize(vertex_num);
    if (mesh_flag & MDL_FLAG_SKIN_BLEND)  mesh.blend_indices.resize(vertex_num);
    if (mesh_flag & MDL_FLAG_SKIN_WEIGHT) mesh.blend_weights.resize(vertex_num);
    if (mesh_flag & MDL_FLAG_UV)          mesh.texcoords.resize(vertex_num);
    if (mesh_flag & MDL_FLAG_UV2)         mesh.texcoord2.resize(vertex_num);

    for (uint32_t i = 0; i < vertex_num; ++i) {
        for (auto& v : mesh.positions[i]) v = f.ReadFloat();
        if (mesh_flag & MDL_FLAG_NORMAL) {
            for (auto& v : mesh.normals[i]) v = f.ReadFloat();
        }
        if (mesh_flag & MDL_FLAG_TANGENT) {
            for (auto& v : mesh.tangents[i]) v = f.ReadFloat();
        }
        if (mesh_flag & MDL_FLAG_EXTRA4) {
            for (auto& v : mesh.extra4[i]) v = f.ReadUint8();
        }
        if (mesh_flag & MDL_FLAG_SKIN_BLEND) {
            for (auto& v : mesh.blend_indices[i]) v = f.ReadUint32();
        }
        if (mesh_flag & MDL_FLAG_SKIN_WEIGHT) {
            for (auto& v : mesh.blend_weights[i]) v = f.ReadFloat();
        }
        if (mesh_flag & MDL_FLAG_UV) {
            for (auto& v : mesh.texcoords[i]) v = f.ReadFloat();
        }
        if (mesh_flag & MDL_FLAG_UV2) {
            for (auto& v : mesh.texcoord2[i]) v = f.ReadFloat();
        }
    }

    uint32_t indices_size = f.ReadUint32();
    if (indices_size % singile_indices != 0) {
        rstd_error("unsupport mdl indices size {} in {}", indices_size, std::string(path));
        return false;
    }
    uint32_t indices_num = indices_size / singile_indices;
    mesh.indices.resize(indices_num);
    for (auto& id : mesh.indices) {
        for (auto& v : id) v = f.ReadUint16();
    }

    // V21+ Parts sub-block (hexpat Parts<MdlV>): optional uv2 region followed
    // by an optional part draw-range list.
    if (header.mdlv >= 21) {
        uint8_t unk_a = f.ReadUint8();
        if (unk_a == 1) {
            uint8_t unk_b = f.ReadUint8();
            if (unk_b) {
                uint16_t unk_c = f.ReadUint16();
                if (unk_c != 0) {
                    rstd_info("mdlv{} parts unk_c expected 0, got {}", header.mdlv, unk_c);
                }
                (void)f.ReadUint8();                        // vert_section_marker
                uint32_t payload_size = f.ReadUint32();
                if (payload_size != 12u * vertex_num) {
                    rstd_error("mdlv{} extras payload size {} != 12*{}",
                               header.mdlv, payload_size, vertex_num);
                    return false;
                }
                mesh.part_uv2.resize(vertex_num);
                mesh.part_uv2_pad.resize(vertex_num);
                for (uint32_t i = 0; i < vertex_num; ++i) {
                    mesh.part_uv2[i][0]  = f.ReadFloat();
                    mesh.part_uv2[i][1]  = f.ReadFloat();
                    mesh.part_uv2_pad[i] = f.ReadUint32();
                }
            }
        } else if (unk_a != 0) {
            rstd_error("mdlv{} parts unhandled unk_a={}", header.mdlv, unk_a);
            return false;
        }
        uint8_t has_parts = f.ReadUint8();
        if (has_parts) {
            uint32_t parts_bytes = f.ReadUint32();
            if (parts_bytes % 16 != 0) {
                rstd_error("mdlv{} parts byte count {} not %% 16", header.mdlv, parts_bytes);
                return false;
            }
            uint32_t parts_num = parts_bytes / 16;
            mesh.parts.resize(parts_num);
            for (auto& part : mesh.parts) {
                part.id    = f.ReadUint32();
                (void)f.ReadUint32(); // reserved 0
                part.start = f.ReadUint32();
                part.size  = f.ReadUint32();
            }
        }
        if (header.mdlv > 21) {
            ParseMasks(f, mesh);
        }
    }
    return true;
}

bool ParseIkConfig(fs::MemBinaryStream& f, WPPuppet::IkConfig& ik) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) ik.chain_a_target(r, c) = f.ReadFloat();
    ik.ik_version   = f.ReadUint8();
    ik.ik_header[0] = f.ReadUint32();
    ik.ik_header[1] = f.ReadUint32();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) ik.chain_b_target(r, c) = f.ReadFloat();
    for (auto& b : ik.ik_flags) b = f.ReadUint8();
    for (auto& v : ik.pole_targets) {
        for (int k = 0; k < 3; ++k) v[k] = f.ReadFloat();
    }
    uint16_t rest_count = f.ReadUint16();
    ik.rest_rotations.resize(rest_count);
    for (auto& br : ik.rest_rotations) {
        br.bone_id = f.ReadUint32();
        for (auto& v : br.dir) v = f.ReadFloat();
    }
    auto read_chain_bone_dir = [&](WPPuppet::ChainBoneDir& d) {
        d.chain_id = f.ReadUint16();
        d.bone_id  = f.ReadUint32();
        for (auto& v : d.dir) v = f.ReadFloat();
    };
    auto read_bone_dir = [&](WPPuppet::BoneDir& d) {
        d.bone_id = f.ReadUint32();
        for (auto& v : d.dir) v = f.ReadFloat();
    };
    ik.ik_targets.resize(6);
    read_chain_bone_dir(ik.ik_targets[0]);
    (void)f.ReadUint16();
    read_chain_bone_dir(ik.ik_targets[1]);
    for (int i = 0; i < 4; ++i) (void)f.ReadUint16();
    read_chain_bone_dir(ik.ik_targets[2]);
    for (int i = 0; i < 3; ++i) (void)f.ReadUint16();
    read_chain_bone_dir(ik.ik_targets[3]);
    auto& root = ik.ik_target_root.emplace();
    read_bone_dir(root);
    read_chain_bone_dir(ik.ik_targets[4]);
    read_chain_bone_dir(ik.ik_targets[5]);
    for (int i = 0; i < 3; ++i) (void)f.ReadUint16();
    ik.ik_constraint.cnt   = f.ReadUint16();
    ik.ik_constraint.id    = f.ReadUint32();
    ik.ik_constraint.child = f.ReadUint32();
    ik.ik_constraint.val   = f.ReadUint32();
    for (auto& lst : ik.ik_bone_lists) {
        uint16_t cnt = f.ReadUint16();
        lst.resize(cnt);
        for (auto& v : lst) v = f.ReadUint32();
    }
    ik.ik_chain_count      = f.ReadUint32();
    ik.ik_chain_length[0]  = f.ReadFloat();
    ik.ik_chain_length[1]  = f.ReadFloat();
    uint16_t chain_bones_cnt = f.ReadUint16();
    ik.ik_chain_bones.resize(chain_bones_cnt);
    for (auto& v : ik.ik_chain_bones) v = f.ReadUint32();
    return true;
}

bool ParseMDLS(fs::MemBinaryStream& f, WPMdl& mdl, std::string_view path) {
    mdl.mdls = ReadMDLVesion(f);

    uint32_t end_offset = f.ReadUint32();

    uint16_t bones_num = f.ReadUint16();
    f.ReadUint16();        // zero pad

    mdl.puppet  = std::make_shared<WPPuppet>();
    auto& bones = mdl.puppet->bones;

    bones.resize(bones_num);
    for (unsigned i = 0; i < bones_num; ++i) {
        auto& bone    = bones[i];
        bone.name     = f.ReadStr();
        bone.sim_type = f.ReadInt32();

        uint32_t file_parent = f.ReadUint32();
        if (file_parent >= i && file_parent != WPPuppet::NO_PARENT) {
            rstd_error("mdl wrong bone parent index {} (i={}) in {}",
                       file_parent, i, std::string(path));
            return false;
        }
        bone.bind_parent = file_parent;
        bone.anim_parent = file_parent;

        uint32_t size = f.ReadUint32();
        if (size != 64) {
            rstd_error("mdl unsupport bones size: {}", size);
            return false;
        }
        for (auto row : bone.local_bind.matrix().colwise()) {
            for (auto& x : row) x = f.ReadFloat();
        }
        bone.simulation_json = f.ReadStr();
    }

    if (mdl.mdls > 1) {
        uint16_t extras_flag = f.ReadUint16();

        if (mdl.mdls == 2) {
            uint8_t has_world_binds = f.ReadUint8();
            if (has_world_binds) {
                // Per-bone world-bind mat4 inline (mdls v2 only).
                for (unsigned i = 0; i < bones_num; ++i)
                    for (unsigned j = 0; j < 16; ++j) f.ReadFloat();
            }
            uint8_t pad[8];
            f.Read(pad, sizeof(pad));
        } else {
            uint8_t zero_b = f.ReadUint8();
            if (zero_b != 0) {
                rstd_info("MDLSv{} zero_b expected 0, got {}", mdl.mdls, zero_b);
            }
            uint32_t pair0 = f.ReadUint32();
            uint32_t pair1 = f.ReadUint32();
            (void)pair0; (void)pair1;

            if (extras_flag == 2) {
                ParseIkConfig(f, mdl.puppet->ik_config.emplace());
            } else if (extras_flag != 0) {
                rstd_info("MDLSv{} unexpected extras_flag {}", mdl.mdls, extras_flag);
            }
        }

        uint8_t has_offset_trans = f.ReadUint8();
        if (has_offset_trans) {
            for (unsigned i = 0; i < bones_num; ++i) {
                auto& b = mdl.puppet->bones[i];
                b.has_file_skin_pivot = true;
                b.file_skin_pivot.x() = f.ReadFloat();
                b.file_skin_pivot.y() = f.ReadFloat();
                b.file_skin_pivot.z() = f.ReadFloat();
                for (int r = 0; r < 4; ++r) {
                    for (int c = 0; c < 4; ++c) {
                        b.file_skin_mat(r, c) = f.ReadFloat();
                    }
                }
            }
        }

        uint8_t has_index = f.ReadUint8();
        if (has_index) {
            for (unsigned i = 0; i < bones_num; ++i) f.ReadUint32();
        }

        if (mdl.mdls >= 3) {
            uint8_t has_depth = f.ReadUint8();
            if (has_depth) {
                for (unsigned i = 0; i < bones_num; ++i) (void)f.ReadUint32();
            }
        }
    }

    // Honour the block's declared end so partial IK / unknown trailer can't
    // poison subsequent MDxx scans.
    if (end_offset > 0 && static_cast<uint32_t>(f.Tell()) != end_offset) {
        rstd_info("MDLS body ended at 0x{:X} but end_offset=0x{:X} ({})",
                  static_cast<uint32_t>(f.Tell()), end_offset, std::string(path));
        f.SeekSet(end_offset);
    }
    return true;
}

void ParseMDAT(fs::MemBinaryStream& f, WPMdl& mdl) {
    uint32_t end_offset      = f.ReadUint32();
    uint32_t num_attachments = f.ReadUint16();
    auto& attachments        = mdl.puppet->attachments;
    attachments.resize(num_attachments);
    for (auto& att : attachments) {
        att.unk  = f.ReadUint16();
        att.name = f.ReadStr();
        for (auto& b : att.data) b = f.ReadUint8();
    }
    if (end_offset > 0 && static_cast<uint32_t>(f.Tell()) != end_offset) {
        f.SeekSet(end_offset);
    }
}

// hexpat AnimBoneCurves: u8 has_curves; if(has_curves) BoneFrameCurve[bone_count].
// Each BoneFrameCurve = u32 zero + u32 byte_size + float[byte_size/4].
bool ParseAnimBoneCurves(fs::MemBinaryStream& f,
                         std::vector<WPPuppet::BoneFrameCurve>& out, uint32_t bone_count) {
    uint8_t has_curves = f.ReadUint8();
    if (! has_curves) return true;
    out.resize(bone_count);
    for (auto& curve : out) {
        uint32_t zero_a = f.ReadUint32();
        if (zero_a != 0) {
            rstd_info("BoneFrameCurve zero_a expected 0, got {}", zero_a);
        }
        uint32_t byte_size = f.ReadUint32();
        if (byte_size % 4 != 0) {
            rstd_error("BoneFrameCurve byte_size {} not %% 4", byte_size);
            return false;
        }
        curve.values.resize(byte_size / 4);
        for (auto& v : curve.values) v = f.ReadFloat();
    }
    return true;
}

bool ParseAnimation(fs::MemBinaryStream& f, WPPuppet::Animation& anim,
                    int mdla_ver, std::string_view path) {
    anim.id           = f.ReadInt32();
    anim.unk_after_id = f.ReadUint32();
    if (anim.id <= 0) {
        rstd_error("wrong anime id {}", anim.id);
        return false;
    }

    anim.name = f.ReadStr();
    if (anim.name.empty()) anim.name = f.ReadStr();

    anim.mode   = ToPlayMode(f.ReadStr());
    anim.fps    = f.ReadFloat();
    anim.length = f.ReadInt32();
    f.ReadInt32(); // anim_zero

    uint32_t b_num = f.ReadUint32();
    anim.bone_tracks.resize(b_num);
    for (uint32_t ti = 0; ti < b_num; ++ti) {
        auto& track      = anim.bone_tracks[ti];
        track.bone_index = ti;            // dense: slot i animates bone i
        track.unk        = f.ReadInt32();
        uint32_t byte_size = f.ReadUint32();
        if (byte_size % singile_bone_frame != 0) {
            rstd_error("wrong bone frame size {} in {}", byte_size, std::string(path));
            return false;
        }
        uint32_t num = byte_size / singile_bone_frame;
        track.frames.resize(num);
        for (auto& frame : track.frames) {
            for (auto& v : frame.position) v = f.ReadFloat();
            for (auto& v : frame.angle)    v = f.ReadFloat();
            for (auto& v : frame.scale)    v = f.ReadFloat();
        }
    }

    if (mdla_ver >= 3) {
        uint32_t trans_flag = f.ReadUint32();
        if (trans_flag == 1) {
            auto& tr = anim.trans.emplace();
            uint32_t extra_size = f.ReadUint32();
            if (extra_size > 0) {
                if (extra_size % 4 != 0) {
                    rstd_error("UnkAnimTrans extra_size {} not %% 4", extra_size);
                    return false;
                }
                tr.extra_track.resize(extra_size / 4);
                for (auto& v : tr.extra_track) v = f.ReadFloat();
                uint32_t extra_zero = f.ReadUint32();
                if (extra_zero != 0) {
                    rstd_info("UnkAnimTrans extra_zero expected 0, got {}", extra_zero);
                }
            }
            uint32_t main_size = f.ReadUint32();
            if (main_size % 4 != 0) {
                rstd_error("UnkAnimTrans main_size {} not %% 4", main_size);
                return false;
            }
            tr.main_track.resize(main_size / 4);
            for (auto& v : tr.main_track) v = f.ReadFloat();
            if (extra_size > 0) {
                uint32_t trail_zero = f.ReadUint32();
                if (trail_zero != 0) {
                    rstd_info("UnkAnimTrans trail_zero expected 0, got {}", trail_zero);
                }
            }
        }
        if (! ParseAnimBoneCurves(f, anim.blend_curves, b_num)) return false;
    }

    if (mdla_ver >= 4) {
        uint8_t has_v4_events = f.ReadUint8();
        if (has_v4_events == 1) {
            uint32_t v4_count = f.ReadUint32();
            anim.v4_events.resize(v4_count);
            for (auto& ev : anim.v4_events) {
                ev.time     = f.ReadFloat();
                ev.flags    = f.ReadUint32();
                uint32_t bs = f.ReadUint32();
                if (bs % 4 != 0) {
                    rstd_error("AnimV4Event byte_size {} not %% 4", bs);
                    return false;
                }
                ev.values.resize(bs / 4);
                for (auto& v : ev.values) v = f.ReadFloat();
            }
        } else if (has_v4_events != 0) {
            rstd_info("Animation has_v4_events expected 0/1, got {}", has_v4_events);
        }
    }

    if (mdla_ver >= 5) {
        for (auto& v : anim.aabb_min) v = f.ReadFloat();
        for (auto& v : anim.aabb_max) v = f.ReadFloat();
        anim.has_aabb = true;
    }

    if (mdla_ver == 6) {
        if (! ParseAnimBoneCurves(f, anim.scalar_curves, b_num)) return false;
    }

    // Trailing event list — present on every animation regardless of mdla
    // version. Pre-mdla>=3 anims start here directly.
    uint32_t event_count = f.ReadUint32();
    anim.events.resize(event_count);
    for (auto& ev : anim.events) {
        ev.time_value = f.ReadUint32();
        ev.event_json = f.ReadStr();
    }
    return true;
}

bool ParseMDLA(fs::MemBinaryStream& f, WPMdl& mdl, std::string_view tag,
               std::string_view path) {
    mdl.mdla = std::stoi(std::string(tag.substr(4, 4)));
    if (mdl.mdla == 0) return true;

    uint32_t end_offset = f.ReadUint32();

    uint32_t anim_num = f.ReadUint32();
    auto& anims = mdl.puppet->anims;
    anims.resize(anim_num);
    for (auto& anim : anims) {
        if (! ParseAnimation(f, anim, mdl.mdla, path)) return false;
    }

    if (end_offset > 0 && static_cast<uint32_t>(f.Tell()) != end_offset) {
        rstd_info("MDLA body ended at 0x{:X} but end_offset=0x{:X} ({})",
                  static_cast<uint32_t>(f.Tell()), end_offset, std::string(path));
        f.SeekSet(end_offset);
    }
    return true;
}

void ParseMasks(fs::MemBinaryStream& f, WPMdl::Mesh& mesh) {
    uint32_t mask_count = f.ReadUint32();
    mesh.masks.resize(mask_count);
    for (auto& m : mesh.masks) {
        m.leading_a = f.ReadUint32();
        uint32_t zero_a = f.ReadUint32();
        if (zero_a != 0) rstd_info("MaskBlock zero_a expected 0, got {}", zero_a);
        m.mat_json = f.ReadStr();
        uint32_t zero_pad = f.ReadUint32();
        if (zero_pad != 0) rstd_info("MaskBlock zero_pad expected 0, got {}", zero_pad);
        uint32_t a_count = f.ReadUint32();
        m.part_ids_a.resize(a_count);
        for (auto& v : m.part_ids_a) v = f.ReadUint32();
        uint32_t b_count = f.ReadUint32();
        m.part_ids_b.resize(b_count);
        for (auto& v : m.part_ids_b) v = f.ReadUint32();
    }
}

bool ParseMDMP(fs::MemBinaryStream& f, WPMdl& mdl, std::string_view tag,
               std::string_view path) {
    mdl.mdmp = std::stoi(std::string(tag.substr(4, 4)));
    uint32_t end_offset = f.ReadUint32();
    while (f.Tell() < end_offset) {
        auto& sec = mdl.morph_sections.emplace_back();
        uint16_t count    = f.ReadUint16();
        sec.event_time    = f.ReadFloat();
        sec.event_id      = f.ReadUint16();
        uint16_t zero_a   = f.ReadUint16();
        if (zero_a != 0) {
            rstd_info("MDMPSection zero_a expected 0, got {}", zero_a);
        }
        sec.sections.resize(count);
        for (auto& sd : sec.sections) {
            sd.shape_id      = f.ReadUint32();
            uint32_t sd_zero = f.ReadUint32();
            if (sd_zero != 0) {
                rstd_info("MDMPSectionData zero_a expected 0, got {}", sd_zero);
            }
            sd.tag           = f.ReadStr();
            uint32_t length  = f.ReadUint32();
            sd.hash          = f.ReadUint32();
            if (length % 6 != 0) {
                rstd_error("MDMPSectionData length {} not %% 6", length);
                return false;
            }
            uint32_t vcount  = length / 6;
            sd.vertices.resize(vcount);
            for (auto& v : sd.vertices) {
                for (auto& x : v) x = f.ReadUint16();
            }
            if (sd.shape_id == 0) {
                sd.trailer.resize(length);
                for (auto& b : sd.trailer) b = f.ReadUint8();
            } else {
                sd.vertex_trailers.resize(vcount);
                for (auto& v : sd.vertex_trailers) v = f.ReadUint16();
            }
        }
    }
    if (end_offset > 0 && static_cast<uint32_t>(f.Tell()) != end_offset) {
        rstd_info("MDMP body ended at 0x{:X} but end_offset=0x{:X} ({})",
                  static_cast<uint32_t>(f.Tell()), end_offset, std::string(path));
        f.SeekSet(end_offset);
    }
    return true;
}

bool ParseMDLE(fs::MemBinaryStream& f, WPMdl& mdl, std::string_view tag) {
    mdl.mdle = std::stoi(std::string(tag.substr(4, 4)));
    uint32_t end_offset    = f.ReadUint32();
    uint32_t payload_bytes = f.ReadUint32();
    const size_t nbones    = mdl.puppet->bones.size();
    const size_t expected  = nbones * 64;
    if (payload_bytes != expected) {
        rstd_error("MDLE payload_bytes {} != bones_num*64 {}", payload_bytes, expected);
        return false;
    }
    for (auto& bone : mdl.puppet->bones) {
        bone.file_world_bind = Eigen::Affine3f::Identity();
        for (auto col : bone.file_world_bind.matrix().colwise()) {
            for (auto& v : col) v = f.ReadFloat();
        }
        bone.has_file_world_bind = true;
    }
    if (end_offset > 0 && static_cast<uint32_t>(f.Tell()) != end_offset) {
        f.SeekSet(end_offset);
    }
    return true;
}

// Skeleton convention changed at MDLS v3 (correlates with MDLV21 in the
// observed corpus, but MDLS is the real signal):
//   - bone.local_bind, frame.position all live in a shared "compact"
//     model space; the file's parent index produces double-translation
//     if composed, so flatten both bind_parent and anim_parent.
//   - Each per-bone sprite is scaled around its vertex centroid (not
//     bone.t); precompute that centroid as an offset for genFrame to
//     bake into the bone's translation. (Matches WE DXBC hash
//     84b2d428-... which emits pure-translation g_Bones with row0/1/2
//     identity at rest and scaled rows during eye-blink frames.)
void ApplyMDLS3CentroidPivot(WPMdl& mdl) {
    if (mdl.meshes.empty()) return;
    for (auto& b : mdl.puppet->bones) {
        b.bind_parent = WPPuppet::NO_PARENT;
        b.anim_parent = WPPuppet::NO_PARENT;
    }
    const size_t nbones = mdl.puppet->bones.size();
    std::vector<Eigen::Vector3d> sum_pos(nbones, Eigen::Vector3d::Zero());
    std::vector<double>          sum_w(nbones, 0.0);
    auto v_to_e = [](const std::array<float, 3>& p) {
        return Eigen::Vector3d { p[0], p[1], p[2] };
    };
    const auto& m0 = mdl.meshes[0];
    if (m0.blend_indices.empty() || m0.blend_weights.empty()) return;
    if (! m0.indices.empty()) {
        for (const auto& tri : m0.indices) {
            if (tri[0] >= m0.positions.size() || tri[1] >= m0.positions.size() ||
                tri[2] >= m0.positions.size()) continue;
            Eigen::Vector3d p0 = v_to_e(m0.positions[tri[0]]);
            Eigen::Vector3d p1 = v_to_e(m0.positions[tri[1]]);
            Eigen::Vector3d p2 = v_to_e(m0.positions[tri[2]]);
            Eigen::Vector3d centroid_tri = (p0 + p1 + p2) / 3.0;
            double area = 0.5 * (p1 - p0).cross(p2 - p0).norm();
            if (area <= 0.0) continue;
            for (int k = 0; k < 3; ++k) {
                if (m0.blend_weights[tri[k]][0] <= 0.0f) continue;
                uint32_t bi = m0.blend_indices[tri[k]][0];
                if (bi >= nbones) continue;
                sum_pos[bi] += centroid_tri * (area / 3.0);
                sum_w[bi]   += area / 3.0;
            }
        }
    } else {
        for (size_t vi = 0; vi < m0.positions.size(); ++vi) {
            Eigen::Vector3d p = v_to_e(m0.positions[vi]);
            for (int k = 0; k < 4; ++k) {
                float w = m0.blend_weights[vi][k];
                uint32_t bi = m0.blend_indices[vi][k];
                if (w > 0.0f && bi < nbones) {
                    sum_pos[bi] += p * (double)w;
                    sum_w[bi]   += (double)w;
                }
            }
        }
    }
    for (size_t i = 0; i < nbones; ++i) {
        if (sum_w[i] > 0.0) {
            Eigen::Vector3f centroid = (sum_pos[i] / sum_w[i]).cast<float>();
            mdl.puppet->bones[i].vertex_centroid_offset =
                centroid - mdl.puppet->bones[i].local_bind.translation();
        }
    }
}

// hexpat Header: VersionTag mdlv + u32 mdl_flag + s32 always_one(==1) + u32 mesh_count.
bool ReadHeaderFromStream(fs::MemBinaryStream& f, WPMdlHeader& h, std::string_view path_for_log) {
    h.mdlv       = ReadMDLVesion(f);
    h.mdl_flag   = f.ReadUint32();
    h.unk_a      = f.ReadUint32();
    h.mesh_count = f.ReadUint32();
    if (h.unk_a != 1) {
        rstd_info("mdl '{}' header always_one={} (expected 1)",
                  std::string(path_for_log), h.unk_a);
    }
    return true;
}

} // namespace

bool WPMdlParser::ParseHeader(std::string_view path, fs::VFS& vfs, WPMdlHeader& h) {
    auto pfile = vfs.Open("/assets/" + std::string(path));
    if (! pfile) return false;
    auto f = fs::MemBinaryStream(*pfile);
    return ReadHeaderFromStream(f, h, path);
}

bool WPMdlParser::Parse(std::string_view path, fs::VFS& vfs, WPMdl& mdl) {
    auto str_path = std::string(path);
    auto pfile    = vfs.Open("/assets/" + str_path);
    if (! pfile) return false;
    auto memfile  = fs::MemBinaryStream(*pfile);
    auto& f = memfile;

    if (! ReadHeaderFromStream(f, mdl.header, str_path)) return false;

    mdl.meshes.resize(mdl.header.mesh_count);
    for (auto& m : mdl.meshes) {
        if (! ParseMesh(f, mdl.header, m, str_path)) return false;
    }

    // Consume the 9-byte VersionTag for blocks whose body parser expects to
    // start at `end_offset`. MDLS reads its tag internally via ReadMDLVesion.
    auto consume_tag = [&]() -> std::string {
        char buf[9] { 0 };
        f.Read(buf, 9);
        return std::string(buf, 8);
    };

    if (peek_block_magic(f, "MDLS")) {
        if (! ParseMDLS(f, mdl, str_path)) return false;
    }
    if (peek_block_magic(f, "MDAT")) {
        (void)consume_tag();
        ParseMDAT(f, mdl);
    }
    if (peek_block_magic(f, "MDLA")) {
        std::string tag = consume_tag();
        if (! ParseMDLA(f, mdl, tag, str_path)) return false;
    }
    if (peek_block_magic(f, "MDMP")) {
        std::string tag = consume_tag();
        if (! ParseMDMP(f, mdl, tag, str_path)) return false;
    }
    if (peek_block_magic(f, "MDLE")) {
        std::string tag = consume_tag();
        if (! ParseMDLE(f, mdl, tag)) return false;
    }

    // hexpat Body: u8 trailing_nul (mdlv>=14). mdlv==13 file end is padded
    // with zeros until EOF.
    if (mdl.header.mdlv >= 14 && f.Tell() < f.Size()) {
        uint8_t trailing_nul = f.ReadUint8();
        if (trailing_nul != 0) {
            rstd_info("mdlv{} trailing_nul expected 0, got {}", mdl.header.mdlv, trailing_nul);
        }
    } else if (mdl.header.mdlv == 13) {
        while (f.Tell() < f.Size()) {
            auto save = f.Tell();
            uint8_t b = f.ReadUint8();
            if (b != 0) { f.SeekSet(save); break; }
        }
    }

    if (mdl.mdls >= 3) ApplyMDLS3CentroidPivot(mdl);

    if (mdl.puppet) mdl.puppet->prepared();

    rstd_info("read puppet: mdlv: {}, nmdls: {}, mdla: {}, mdle: {}, bones: {}, anims: {}",
              mdl.header.mdlv, mdl.mdls, mdl.mdla, mdl.mdle,
              mdl.puppet ? mdl.puppet->bones.size() : 0,
              mdl.puppet ? mdl.puppet->anims.size() : 0);
    return true;
}

void WPMdlParser::GenMeshFromMdl(SceneMesh& mesh, const WPMdl::Mesh& src) {
    const size_t vert_num = src.positions.size();
    if (vert_num == 0) return;

    // Build the attribute list in a stable order. Skinning attrs come early so
    // a puppet vertex layout matches what WE shaders historically expect.
    std::vector<VertexAttrSpec> specs;
    std::vector<std::function<void(size_t, float*)>> packers;

    // Position is always present (the parser would have failed otherwise).
    specs.push_back(VAttr::Position);
    packers.push_back([&src](size_t i, float* dst) {
        std::memcpy(dst, src.positions[i].data(), sizeof(src.positions[i]));
    });
    if (! src.blend_indices.empty()) {
        specs.push_back(VAttr::BlendIndices);
        packers.push_back([&src](size_t i, float* dst) {
            std::memcpy(dst, src.blend_indices[i].data(), sizeof(src.blend_indices[i]));
        });
    }
    if (! src.blend_weights.empty()) {
        specs.push_back(VAttr::BlendWeights);
        packers.push_back([&src](size_t i, float* dst) {
            std::memcpy(dst, src.blend_weights[i].data(), sizeof(src.blend_weights[i]));
        });
    }
    if (! src.texcoords.empty()) {
        specs.push_back(VAttr::TexCoord);
        packers.push_back([&src](size_t i, float* dst) {
            std::memcpy(dst, src.texcoords[i].data(), sizeof(src.texcoords[i]));
        });
    }

    auto attrs = MakeAttrSet(specs);
    SceneVertexArray vertex(attrs, vert_num);

    size_t stride_floats = 0;
    for (auto& a : attrs) stride_floats += SceneVertexArray::RealAttributeSize(a);
    std::vector<float> one_vert(stride_floats);

    for (size_t i = 0; i < vert_num; ++i) {
        size_t offset = 0;
        for (size_t k = 0; k < packers.size(); ++k) {
            packers[k](i, one_vert.data() + offset);
            offset += SceneVertexArray::RealAttributeSize(attrs[k]);
        }
        vertex.SetVertexs(i, std::span<const float>(one_vert));
    }

    std::vector<uint32_t> indices;
    indices.reserve(src.indices.size() * 3);
    for (const auto& tri : src.indices) {
        for (uint16_t v : tri) indices.push_back(v);
    }

    mesh.AddVertexArray(std::move(vertex));
    mesh.AddIndexArray(SceneIndexArray(std::span<const uint32_t>(indices)));

    // V21 parts[] enumerates index sub-ranges in artist-chosen z-order. We
    // issue one DrawIndexed per range so each "part" is drawn as a separate
    // primitive batch, which lets later parts overdraw earlier ones (eyelid
    // covering pupil at peak blink) and leaves headroom for per-part state.
    if (! src.parts.empty()) {
        std::vector<SceneMesh::DrawRange> ranges;
        ranges.reserve(src.parts.size());
        for (const auto& p : src.parts) {
            if (p.size == 0) continue;
            ranges.push_back({ p.start, p.size });
        }
        mesh.SetDrawRanges(std::move(ranges));
    }
}

void WPMdlParser::AddPuppetShaderInfo(WPShaderInfo& info, const WPMdl& mdl) {
    info.combos["SKINNING"]  = "1";
    info.combos["BONECOUNT"] = std::to_string(mdl.puppet->bones.size());
}

void WPMdlParser::AddPuppetMatInfo(wpscene::WPMaterial& mat, const WPMdl& mdl) {
    mat.combos["SKINNING"]  = 1;
    mat.combos["BONECOUNT"] = (i32)mdl.puppet->bones.size();
    mat.use_puppet          = true;
}
