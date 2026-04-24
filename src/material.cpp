// -----------------------------------------------------------------------------
// material.cpp — PNG → TextureSlot loader, Material teardown.
// -----------------------------------------------------------------------------

#include "material.h"

#include "stb_image.h"

#include <cstdio>

bool load_texture_png(const std::string& path, TextureSlot& slot) {
    int w = 0, h = 0, c = 0;
    uint8_t* px = stbi_load(path.c_str(), &w, &h, &c, 4);
    if (!px) return false;

    sg_image_desc id{};
    id.width  = w;
    id.height = h;
    id.pixel_format = SG_PIXELFORMAT_RGBA8;
    id.data.mip_levels[0] = { px, (size_t)(w * h * 4) };
    slot.image = sg_make_image(&id);

    sg_view_desc vd{};
    vd.texture.image = slot.image;
    slot.view = sg_make_view(&vd);

    stbi_image_free(px);
    slot.valid = (sg_query_image_state(slot.image) == SG_RESOURCESTATE_VALID);
    if (!slot.valid) {
        std::fprintf(stderr, "[material] GPU image creation failed for '%s'\n",
                     path.c_str());
    }
    return slot.valid;
}

void Material::destroy() {
    auto free_slot = [](TextureSlot& s) {
        if (s.valid) {
            sg_destroy_view(s.view);
            sg_destroy_image(s.image);
            s.valid = false;
        }
    };
    free_slot(diffuse);
    free_slot(spec);
    free_slot(glow);
    free_slot(normal);
}
