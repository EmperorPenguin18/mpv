/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "dmabuf_interop.h"

#include <drm_fourcc.h>
#include <EGL/egl.h>
#ifndef EGL_EGLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES
#endif
#include <EGL/eglext.h>
#include "video/out/opengl/ra_gl.h"

typedef void* GLeglImageOES;
typedef void *EGLImageKHR;

// Any EGL_EXT_image_dma_buf_import definitions used in this source file.
#define EGL_LINUX_DMA_BUF_EXT             0x3270
#define EGL_LINUX_DRM_FOURCC_EXT          0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT         0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT     0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT      0x3274
#define EGL_DMA_BUF_PLANE1_FD_EXT         0x3275
#define EGL_DMA_BUF_PLANE1_OFFSET_EXT     0x3276
#define EGL_DMA_BUF_PLANE1_PITCH_EXT      0x3277
#define EGL_DMA_BUF_PLANE2_FD_EXT         0x3278
#define EGL_DMA_BUF_PLANE2_OFFSET_EXT     0x3279
#define EGL_DMA_BUF_PLANE2_PITCH_EXT      0x327A


// Any EGL_EXT_image_dma_buf_import definitions used in this source file.
#define EGL_DMA_BUF_PLANE3_FD_EXT         0x3440
#define EGL_DMA_BUF_PLANE3_OFFSET_EXT     0x3441
#define EGL_DMA_BUF_PLANE3_PITCH_EXT      0x3442
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#define EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT 0x3445
#define EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT 0x3446
#define EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT 0x3447
#define EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT 0x3448
#define EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT 0x3449
#define EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT 0x344A

struct vaapi_gl_mapper_priv {
    GLuint gl_textures[4];
    EGLImageKHR images[4];

    EGLImageKHR (EGLAPIENTRY *CreateImageKHR)(EGLDisplay, EGLContext,
                                              EGLenum, EGLClientBuffer,
                                              const EGLint *);
    EGLBoolean (EGLAPIENTRY *DestroyImageKHR)(EGLDisplay, EGLImageKHR);
    void (EGLAPIENTRY *EGLImageTargetTexture2DOES)(GLenum, GLeglImageOES);
    EGLBoolean (EGLAPIENTRY *QueryDmaBufFormatsEXT)(EGLDisplay, EGLint, EGLint*, EGLint*);
    EGLBoolean (EGLAPIENTRY *QueryDmaBufModifiersEXT)(EGLDisplay, EGLint, EGLint, EGLuint64KHR*, EGLBoolean*, EGLint*);
};

static bool vaapi_gl_mapper_init(struct ra_hwdec_mapper *mapper,
                                 const struct ra_imgfmt_desc *desc)
{
    struct dmabuf_interop_priv *p_mapper = mapper->priv;
    struct vaapi_gl_mapper_priv *p = talloc_ptrtype(NULL, p);
    p_mapper->interop_mapper_priv = p;

    *p = (struct vaapi_gl_mapper_priv) {
        // EGL_KHR_image_base
        .CreateImageKHR = (void *)eglGetProcAddress("eglCreateImageKHR"),
        .DestroyImageKHR = (void *)eglGetProcAddress("eglDestroyImageKHR"),
        // GL_OES_EGL_image
        .EGLImageTargetTexture2DOES =
            (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES"),
	.QueryDmaBufFormatsEXT = (void *)eglGetProcAddress("eglQueryDmaBufFormatsEXT"),
        .QueryDmaBufModifiersEXT = (void *)eglGetProcAddress("eglQueryDmaBufModifiersEXT"),
    };

    if (!p->CreateImageKHR || !p->DestroyImageKHR ||
        !p->EGLImageTargetTexture2DOES ||
	!p->QueryDmaBufFormatsEXT || !p->QueryDmaBufModifiersEXT)
        return false;

    GL *gl = ra_gl_get(mapper->ra);
    gl->GenTextures(4, p->gl_textures);
    for (int n = 0; n < desc->num_planes; n++) {
        gl->BindTexture(GL_TEXTURE_2D, p->gl_textures[n]);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->BindTexture(GL_TEXTURE_2D, 0);

        struct ra_tex_params params = {
            .dimensions = 2,
            .w = mp_image_plane_w(&p_mapper->layout, n),
            .h = mp_image_plane_h(&p_mapper->layout, n),
            .d = 1,
            .format = desc->planes[n],
            .render_src = true,
            .src_linear = true,
        };

        if (params.format->ctype != RA_CTYPE_UNORM)
            return false;

        p_mapper->tex[n] = ra_create_wrapped_tex(mapper->ra, &params,
                                                 p->gl_textures[n]);
        if (!p_mapper->tex[n])
            return false;
    }

    return true;
}

static void vaapi_gl_mapper_uninit(const struct ra_hwdec_mapper *mapper)
{
    struct dmabuf_interop_priv *p_mapper = mapper->priv;
    struct vaapi_gl_mapper_priv *p = p_mapper->interop_mapper_priv;

    if (p) {
        GL *gl = ra_gl_get(mapper->ra);
        gl->DeleteTextures(4, p->gl_textures);
        for (int n = 0; n < 4; n++) {
            p->gl_textures[n] = 0;
            ra_tex_free(mapper->ra, &p_mapper->tex[n]);
        }
        talloc_free(p);
        p_mapper->interop_mapper_priv = NULL;
    }
}

#define ADD_ATTRIB(name, value)                         \
    do {                                                \
    assert(num_attribs + 3 < MP_ARRAY_SIZE(attribs));   \
    attribs[num_attribs++] = (name);                    \
    attribs[num_attribs++] = (value);                   \
    attribs[num_attribs] = EGL_NONE;                    \
    } while(0)

#define ADD_PLANE_ATTRIBS(plane) do { \
            uint64_t drm_format_modifier = p_mapper->desc.objects[p_mapper->desc.layers[i].planes[j].object_index].format_modifier; \
            ADD_ATTRIB(EGL_DMA_BUF_PLANE ## plane ## _FD_EXT, \
                        p_mapper->desc.objects[p_mapper->desc.layers[i].planes[j].object_index].fd); \
            ADD_ATTRIB(EGL_DMA_BUF_PLANE ## plane ## _OFFSET_EXT, \
                        p_mapper->desc.layers[i].planes[j].offset); \
            ADD_ATTRIB(EGL_DMA_BUF_PLANE ## plane ## _PITCH_EXT, \
                        p_mapper->desc.layers[i].planes[j].pitch); \
            if (dmabuf_interop->use_modifiers && drm_format_modifier != DRM_FORMAT_MOD_INVALID) { \
                ADD_ATTRIB(EGL_DMA_BUF_PLANE ## plane ## _MODIFIER_LO_EXT, drm_format_modifier & 0xfffffffful); \
                ADD_ATTRIB(EGL_DMA_BUF_PLANE ## plane ## _MODIFIER_HI_EXT, drm_format_modifier >> 32); \
            }                               \
        } while (0)

static const char* eglErrorString(EGLint error) {
	switch(error) {
		case EGL_SUCCESS: return "EGL_SUCCESS";
		case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
		case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
		case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
		case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
		case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
		case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
		case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
		case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
		case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
		case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
		case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
		case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
		case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
		case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
	}
	return "Unknown error";
}

static const char* drm_string(uint64_t drm, unsigned int bpl) {
	if (drm == DRM_FORMAT_BIG_ENDIAN) return "DRM_FORMAT_BIG_ENDIAN";
	if (drm == DRM_FORMAT_INVALID) return "DRM_FORMAT_INVALID";
	if (drm == DRM_FORMAT_C8) return "DRM_FORMAT_C8";
	if (drm == DRM_FORMAT_R8) return "DRM_FORMAT_R8";
	if (drm == DRM_FORMAT_R10) return "DRM_FORMAT_R10";
	if (drm == DRM_FORMAT_R12) return "DRM_FORMAT_R12";
	if (drm == DRM_FORMAT_R16) return "DRM_FORMAT_R16";
	if (drm == DRM_FORMAT_RG88) return "DRM_FORMAT_RG88";
	if (drm == DRM_FORMAT_GR88) return "DRM_FORMAT_GR88";
	if (drm == DRM_FORMAT_RG1616) return "DRM_FORMAT_RG1616";
	if (drm == DRM_FORMAT_GR1616) return "DRM_FORMAT_GR1616";
	if (drm == DRM_FORMAT_RGB332) return "DRM_FORMAT_RGB332";
	if (drm == DRM_FORMAT_BGR233) return "DRM_FORMAT_BGR233";
	if (drm == DRM_FORMAT_XRGB4444) return "DRM_FORMAT_XRGB4444";
	if (drm == DRM_FORMAT_XBGR4444) return "DRM_FORMAT_XBGR4444";
	if (drm == DRM_FORMAT_RGBX4444) return "DRM_FORMAT_RGBX4444";
	if (drm == DRM_FORMAT_BGRX4444) return "DRM_FORMAT_BGRX4444";
	if (drm == DRM_FORMAT_ARGB4444) return "DRM_FORMAT_ARGB4444";
	if (drm == DRM_FORMAT_ABGR4444) return "DRM_FORMAT_ABGR4444";
	if (drm == DRM_FORMAT_RGBA4444) return "DRM_FORMAT_RGBA4444";
	if (drm == DRM_FORMAT_BGRA4444) return "DRM_FORMAT_BGRA4444";
	if (drm == DRM_FORMAT_XRGB1555) return "DRM_FORMAT_XRGB1555";
	if (drm == DRM_FORMAT_XBGR1555) return "DRM_FORMAT_XBGR1555";
	if (drm == DRM_FORMAT_RGBX5551) return "DRM_FORMAT_RGBX5551";
	if (drm == DRM_FORMAT_BGRX5551) return "DRM_FORMAT_BGRX5551";
	if (drm == DRM_FORMAT_ARGB1555) return "DRM_FORMAT_ARGB1555";
	if (drm == DRM_FORMAT_ABGR1555) return "DRM_FORMAT_ABGR1555";
	if (drm == DRM_FORMAT_RGBA5551) return "DRM_FORMAT_RGBA5551";
	if (drm == DRM_FORMAT_BGRA5551) return "DRM_FORMAT_BGRA5551";
	if (drm == DRM_FORMAT_RGB565) return "DRM_FORMAT_RGB565";
	if (drm == DRM_FORMAT_BGR565) return "DRM_FORMAT_BGR565";
	if (drm == DRM_FORMAT_RGB888) return "DRM_FORMAT_RGB888";
	if (drm == DRM_FORMAT_BGR888) return "DRM_FORMAT_BGR888";
	if (drm == DRM_FORMAT_XRGB8888) return "DRM_FORMAT_XRGB8888";
	if (drm == DRM_FORMAT_XBGR8888) return "DRM_FORMAT_XBGR8888";
	if (drm == DRM_FORMAT_RGBX8888) return "DRM_FORMAT_RGBX8888";
	if (drm == DRM_FORMAT_BGRX8888) return "DRM_FORMAT_BGRX8888";
	if (drm == DRM_FORMAT_ARGB8888) return "DRM_FORMAT_ARGB8888";
	if (drm == DRM_FORMAT_ABGR8888) return "DRM_FORMAT_ABGR8888";
	if (drm == DRM_FORMAT_RGBA8888) return "DRM_FORMAT_RGBA8888";
	if (drm == DRM_FORMAT_BGRA8888) return "DRM_FORMAT_BGRA8888";
	if (drm == DRM_FORMAT_XRGB2101010) return "DRM_FORMAT_XRGB2101010";
	if (drm == DRM_FORMAT_XBGR2101010) return "DRM_FORMAT_XBGR2101010";
	if (drm == DRM_FORMAT_RGBX1010102) return "DRM_FORMAT_RGBX1010102";
	if (drm == DRM_FORMAT_BGRX1010102) return "DRM_FORMAT_BGRX1010102";
	if (drm == DRM_FORMAT_ARGB2101010) return "DRM_FORMAT_ARGB2101010";
	if (drm == DRM_FORMAT_ABGR2101010) return "DRM_FORMAT_ABGR2101010";
	if (drm == DRM_FORMAT_RGBA1010102) return "DRM_FORMAT_RGBA1010102";
	if (drm == DRM_FORMAT_BGRA1010102) return "DRM_FORMAT_BGRA1010102";
	if (drm == DRM_FORMAT_XRGB16161616) return "DRM_FORMAT_XRGB16161616";
	if (drm == DRM_FORMAT_XBGR16161616) return "DRM_FORMAT_XBGR16161616";
	if (drm == DRM_FORMAT_ARGB16161616) return "DRM_FORMAT_ARGB16161616";
	if (drm == DRM_FORMAT_ABGR16161616) return "DRM_FORMAT_ABGR16161616";
	if (drm == DRM_FORMAT_XRGB16161616F) return "DRM_FORMAT_XRGB16161616F";
	if (drm == DRM_FORMAT_XBGR16161616F) return "DRM_FORMAT_XBGR16161616F";
	if (drm == DRM_FORMAT_ARGB16161616F) return "DRM_FORMAT_ARGB16161616F";
	if (drm == DRM_FORMAT_ABGR16161616F) return "DRM_FORMAT_ABGR16161616F";
	if (drm == DRM_FORMAT_AXBXGXRX106106106106) return "DRM_FORMAT_AXBXGXRX106106106106";
	if (drm == DRM_FORMAT_YUYV) return "DRM_FORMAT_YUYV";
	if (drm == DRM_FORMAT_YVYU) return "DRM_FORMAT_YVYU";
	if (drm == DRM_FORMAT_UYVY) return "DRM_FORMAT_UYVY";
	if (drm == DRM_FORMAT_VYUY) return "DRM_FORMAT_VYUY";
	if (drm == DRM_FORMAT_AYUV) return "DRM_FORMAT_AYUV";
	if (drm == DRM_FORMAT_XYUV8888) return "DRM_FORMAT_XYUV8888";
	if (drm == DRM_FORMAT_VUY888) return "DRM_FORMAT_VUY888";
	if (drm == DRM_FORMAT_VUY101010) return "DRM_FORMAT_VUY101010";
	if (drm == DRM_FORMAT_Y210) return "DRM_FORMAT_Y210";
	if (drm == DRM_FORMAT_Y212) return "DRM_FORMAT_Y212";
	if (drm == DRM_FORMAT_Y216) return "DRM_FORMAT_Y216";
	if (drm == DRM_FORMAT_Y410) return "DRM_FORMAT_Y410";
	if (drm == DRM_FORMAT_Y412) return "DRM_FORMAT_Y412";
	if (drm == DRM_FORMAT_Y416) return "DRM_FORMAT_Y416";
	if (drm == DRM_FORMAT_XVYU2101010) return "DRM_FORMAT_XVYU2101010";
	if (drm == DRM_FORMAT_XVYU12_16161616) return "DRM_FORMAT_XVYU12_16161616";
	if (drm == DRM_FORMAT_XVYU16161616) return "DRM_FORMAT_XVYU16161616";
	if (drm == DRM_FORMAT_Y0L0) return "DRM_FORMAT_Y0L0";
	if (drm == DRM_FORMAT_X0L0) return "DRM_FORMAT_X0L0";
	if (drm == DRM_FORMAT_Y0L2) return "DRM_FORMAT_Y0L2";
	if (drm == DRM_FORMAT_X0L2) return "DRM_FORMAT_X0L2";
	if (drm == DRM_FORMAT_YUV420_8BIT) return "DRM_FORMAT_YUV420_8BIT";
	if (drm == DRM_FORMAT_YUV420_10BIT) return "DRM_FORMAT_YUV420_10BIT";
	if (drm == DRM_FORMAT_XRGB8888_A8) return "DRM_FORMAT_XRGB8888_A8";
	if (drm == DRM_FORMAT_XBGR8888_A8) return "DRM_FORMAT_XBGR8888_A8";
	if (drm == DRM_FORMAT_RGBX8888_A8) return "DRM_FORMAT_RGBX8888_A8";
	if (drm == DRM_FORMAT_BGRX8888_A8) return "DRM_FORMAT_BGRX8888_A8";
	if (drm == DRM_FORMAT_RGB888_A8) return "DRM_FORMAT_RGB888_A8";
	if (drm == DRM_FORMAT_BGR888_A8) return "DRM_FORMAT_BGR888_A8";
	if (drm == DRM_FORMAT_RGB565_A8) return "DRM_FORMAT_RGB565_A8";
	if (drm == DRM_FORMAT_BGR565_A8) return "DRM_FORMAT_BGR565_A8";
	if (drm == DRM_FORMAT_NV12) return "DRM_FORMAT_NV12";
	if (drm == DRM_FORMAT_NV21) return "DRM_FORMAT_NV21";
	if (drm == DRM_FORMAT_NV16) return "DRM_FORMAT_NV16";
	if (drm == DRM_FORMAT_NV61) return "DRM_FORMAT_NV61";
	if (drm == DRM_FORMAT_NV24) return "DRM_FORMAT_NV24";
	if (drm == DRM_FORMAT_NV42) return "DRM_FORMAT_NV42";
	if (drm == DRM_FORMAT_NV15) return "DRM_FORMAT_NV15";
	if (drm == DRM_FORMAT_P210) return "DRM_FORMAT_P210";
	if (drm == DRM_FORMAT_P010) return "DRM_FORMAT_P010";
	if (drm == DRM_FORMAT_P012) return "DRM_FORMAT_P012";
	if (drm == DRM_FORMAT_P016) return "DRM_FORMAT_P016";
	if (drm == DRM_FORMAT_P030) return "DRM_FORMAT_P030";
	if (drm == DRM_FORMAT_Q410) return "DRM_FORMAT_Q410";
	if (drm == DRM_FORMAT_Q401) return "DRM_FORMAT_Q401";
	if (drm == DRM_FORMAT_YUV410) return "DRM_FORMAT_YUV410";
	if (drm == DRM_FORMAT_YVU410) return "DRM_FORMAT_YVU410";
	if (drm == DRM_FORMAT_YUV411) return "DRM_FORMAT_YUV411";
	if (drm == DRM_FORMAT_YVU411) return "DRM_FORMAT_YVU411";
	if (drm == DRM_FORMAT_YUV420) return "DRM_FORMAT_YUV420";
	if (drm == DRM_FORMAT_YVU420) return "DRM_FORMAT_YVU420";
	if (drm == DRM_FORMAT_YUV422) return "DRM_FORMAT_YUV422";
	if (drm == DRM_FORMAT_YVU422) return "DRM_FORMAT_YVU422";
	if (drm == DRM_FORMAT_YUV444) return "DRM_FORMAT_YUV444";
	if (drm == DRM_FORMAT_YVU444) return "DRM_FORMAT_YVU444";
		//case DRM_FORMAT_MOD_VENDOR_NONE: return "DRM_FORMAT_MOD_VENDOR_NONE";
	if (drm == DRM_FORMAT_MOD_VENDOR_INTEL) return "DRM_FORMAT_MOD_VENDOR_INTEL";
	if (drm == DRM_FORMAT_MOD_VENDOR_AMD) return "DRM_FORMAT_MOD_VENDOR_AMD";
	if (drm == DRM_FORMAT_MOD_VENDOR_NVIDIA) return "DRM_FORMAT_MOD_VENDOR_NVIDIA";
	if (drm == DRM_FORMAT_MOD_VENDOR_SAMSUNG) return "DRM_FORMAT_MOD_VENDOR_SAMSUNG";
	if (drm == DRM_FORMAT_MOD_VENDOR_QCOM) return "DRM_FORMAT_MOD_VENDOR_QCOM";
	if (drm == DRM_FORMAT_MOD_VENDOR_VIVANTE) return "DRM_FORMAT_MOD_VENDOR_VIVANTE";
	if (drm == DRM_FORMAT_MOD_VENDOR_BROADCOM) return "DRM_FORMAT_MOD_VENDOR_BROADCOM";
	if (drm == DRM_FORMAT_MOD_VENDOR_ARM) return "DRM_FORMAT_MOD_VENDOR_ARM";
	if (drm == DRM_FORMAT_MOD_VENDOR_ALLWINNER) return "DRM_FORMAT_MOD_VENDOR_ALLWINNER";
	if (drm == DRM_FORMAT_MOD_VENDOR_AMLOGIC) return "DRM_FORMAT_MOD_VENDOR_AMLOGIC";
		//case DRM_FORMAT_RESERVED: return "DRM_FORMAT_RESERVED";
	if (drm == DRM_FORMAT_MOD_GENERIC_16_16_TILE) return "DRM_FORMAT_MOD_GENERIC_16_16_TILE";
	if (drm == DRM_FORMAT_MOD_INVALID) return "DRM_FORMAT_MOD_INVALID";
		//case DRM_FORMAT_MOD_LINEAR: return "DRM_FORMAT_MOD_LINEAR";
		//case DRM_FORMAT_MOD_NONE: return "DRM_FORMAT_MOD_NONE";
	if (drm == DRM_FORMAT_MOD_SAMSUNG_64_32_TILE) return "DRM_FORMAT_MOD_SAMSUNG_64_32_TILE";
		//case DRM_FORMAT_MOD_SAMSUNG_16_16_TILE: return "DRM_FORMAT_MOD_SAMSUNG_16_16_TILE";
	if (drm == DRM_FORMAT_MOD_QCOM_COMPRESSED) return "DRM_FORMAT_MOD_QCOM_COMPRESSED";
	if (drm == DRM_FORMAT_MOD_QCOM_TILED3) return "DRM_FORMAT_MOD_QCOM_TILED3";
	if (drm == DRM_FORMAT_MOD_QCOM_TILED2) return "DRM_FORMAT_MOD_QCOM_TILED2";
	if (drm == DRM_FORMAT_MOD_VIVANTE_TILED) return "DRM_FORMAT_MOD_VIVANTE_TILED";
	if (drm == DRM_FORMAT_MOD_VIVANTE_SUPER_TILED) return "DRM_FORMAT_MOD_VIVANTE_SUPER_TILED";
	if (drm == DRM_FORMAT_MOD_VIVANTE_SPLIT_TILED) return "DRM_FORMAT_MOD_VIVANTE_SPLIT_TILED";
	if (drm == DRM_FORMAT_MOD_VIVANTE_SPLIT_SUPER_TILED) return "DRM_FORMAT_MOD_VIVANTE_SPLIT_SUPER_TILED";
	if (drm == DRM_FORMAT_MOD_NVIDIA_TEGRA_TILED) return "DRM_FORMAT_MOD_NVIDIA_TEGRA_TILED";
	if (drm == DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK_ONE_GOB) return "DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK_ONE_GOB";
	if (drm == DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK_TWO_GOB) return "DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK_TWO_GOB";
	if (drm == DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK_FOUR_GOB) return "DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK_FOUR_GOB";
	if (drm == DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK_EIGHT_GOB) return "DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK_EIGHT_GOB";
	if (drm == DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK_SIXTEEN_GOB) return "DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK_SIXTEEN_GOB";
	if (drm == DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK_THIRTYTWO_GOB) return "DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK_THIRTYTWO_GOB";
	if (drm == DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED) return "DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED";
	if (drm == DRM_FORMAT_MOD_BROADCOM_SAND32) return "DRM_FORMAT_MOD_BROADCOM_SAND32";
	if (drm == DRM_FORMAT_MOD_BROADCOM_SAND32_COL_HEIGHT(bpl)) return "DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT";
	if (drm == DRM_FORMAT_MOD_BROADCOM_SAND64) return "DRM_FORMAT_MOD_BROADCOM_SAND64";
	if (drm == DRM_FORMAT_MOD_BROADCOM_SAND64_COL_HEIGHT(bpl)) return "DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT";
	if (drm == DRM_FORMAT_MOD_BROADCOM_SAND128) return "DRM_FORMAT_MOD_BROADCOM_SAND128";
	if (drm == DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT(bpl)) return "DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT";
	if (drm == DRM_FORMAT_MOD_BROADCOM_SAND256) return "DRM_FORMAT_MOD_BROADCOM_SAND256";
	if (drm == DRM_FORMAT_MOD_BROADCOM_SAND256_COL_HEIGHT(bpl)) return "DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT";
	if (drm == DRM_FORMAT_MOD_BROADCOM_UIF) return "DRM_FORMAT_MOD_BROADCOM_UIF";
		//case DRM_FORMAT_MOD_ARM_TYPE_AFBC: return "DRM_FORMAT_MOD_ARM_TYPE_AFBC";
		//case DRM_FORMAT_MOD_ARM_TYPE_MISC: return "DRM_FORMAT_MOD_ARM_TYPE_MISC";
		//case DRM_FORMAT_MOD_ARM_TYPE_AFRC: return "DRM_FORMAT_MOD_ARM_TYPE_AFRC";
	if (drm == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED) return "DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED";
	if (drm == DRM_FORMAT_MOD_ALLWINNER_TILED) return "DRM_FORMAT_MOD_ALLWINNER_TILED";
	return "unknown";
}

static bool check_modifier(struct vaapi_gl_mapper_priv* p, uint32_t drm_format, uint64_t drm_format_modifier, unsigned int bpl) {
    printf("format: %s\n", drm_string(drm_format, bpl));
    printf("modifier: %s\n", drm_string(drm_format_modifier, bpl));
    EGLint* formats = NULL;
    EGLuint64KHR* modifiers = NULL;
    EGLint num;
    if (!p->QueryDmaBufFormatsEXT(eglGetCurrentDisplay(), 0, NULL, &num)) {
	    fprintf(stderr, "Function failed\n");
	    goto failed;
    }
    if (num < 1) {
	    fprintf(stderr, "Num too small\n");
	    goto failed;
    }
    formats = malloc(num*sizeof(EGLint));
    if (!p->QueryDmaBufFormatsEXT(eglGetCurrentDisplay(), num, formats, &num)) {
	    fprintf(stderr, "Function failed\n");
	    goto failed;
    }
    EGLint format = 0;
    for (int i = 0; i < num; i++) {
	    if (formats[i] == drm_format) {
		    format = formats[i];
		    break;
	    } else if (i == num-1) {
		    fprintf(stderr, "Format not found\n");
		    goto failed;
	    }
    }
    free(formats); formats = NULL;
    if (!p->QueryDmaBufModifiersEXT(eglGetCurrentDisplay(), format, 0, NULL, NULL, &num)) {
	    fprintf(stderr, "Function failed\n");
	    goto failed;
    }
    if (num < 1) {
	    fprintf(stderr, "Num too small\n");
	    goto failed;
    }
    modifiers = malloc(num*sizeof(EGLuint64KHR));
    if (!p->QueryDmaBufModifiersEXT(eglGetCurrentDisplay(), format, num, modifiers, NULL, &num)) {
	    fprintf(stderr, "Function failed\n");
	    goto failed;
    }
    EGLuint64KHR modifier = 0;
    for (int i = 0; i < num; i++) {
    	    printf("list: %s\n", drm_string(modifiers[i], bpl));
	    if (modifiers[i] == drm_format_modifier) {
		    modifier = modifiers[i];
		    break;
	    } else if (i == num-1) {
		    fprintf(stderr, "Modifier not found\n");
		    goto failed;
	    }
    }
    free(modifiers); modifiers = NULL;
    return true;
failed:
    if (formats) free(formats);
    if (modifiers) free(modifiers);
    return false;
}

static bool vaapi_gl_map(struct ra_hwdec_mapper *mapper,
                         struct dmabuf_interop *dmabuf_interop,
                         bool probing)
{
    struct dmabuf_interop_priv *p_mapper = mapper->priv;
    struct vaapi_gl_mapper_priv *p = p_mapper->interop_mapper_priv;

    GL *gl = ra_gl_get(mapper->ra);

    for (int i = 0, n = 0; i < p_mapper->desc.nb_layers; i++) {
        /*
         * As we must map surfaces as one texture per plane, we can only support
         * a subset of possible multi-plane layer formats. This is due to having
         * to manually establish what DRM format each synthetic layer should
         * have.
         */
        uint32_t format[AV_DRM_MAX_PLANES] = {
            p_mapper->desc.layers[i].format,
        };

        if (p_mapper->desc.layers[i].nb_planes > 1) {
            switch (p_mapper->desc.layers[i].format) {
            case DRM_FORMAT_NV12:
		//check_modifier(p, DRM_FORMAT_NV12, DRM_FORMAT_MOD_BROADCOM_SAND128, 1200);
		//check_modifier(p, DRM_FORMAT_R8, DRM_FORMAT_MOD_BROADCOM_SAND128, 1200);
		//check_modifier(p, DRM_FORMAT_GR88, DRM_FORMAT_MOD_BROADCOM_SAND128, 1200);
                format[0] = DRM_FORMAT_R8;
                format[1] = DRM_FORMAT_GR88;
            	//p_mapper->desc.layers[i].planes[0].pitch = 128; //maybe?
            	//p_mapper->desc.layers[i].planes[1].pitch = 128; //maybe?
            	//p_mapper->desc.layers[i].planes[0].offset = 102400; //maybe?
            	//p_mapper->desc.layers[i].planes[1].offset = 0; //maybe?
	        //p_mapper->desc.objects[p_mapper->desc.layers[i].planes[0].object_index].format_modifier = DRM_FORMAT_MOD_BROADCOM_UIF; //maybe?
		/*for (int a = 1; a < 102401; a++) {
			const char* str = drm_string(p_mapper->desc.objects[p_mapper->desc.layers[i].planes[0].object_index].format_modifier, a); 
			if (strcmp(str, "unknown") != 0) printf("%s(%d)\n", str, a);
		}*/
                break;
            case DRM_FORMAT_YUV420:
                format[0] = DRM_FORMAT_R8;
                format[1] = DRM_FORMAT_R8;
                format[2] = DRM_FORMAT_R8;
                break;
            case DRM_FORMAT_P010:
                format[0] = DRM_FORMAT_R16;
                format[1] = DRM_FORMAT_GR1616;
                break;
            default:
                mp_msg(mapper->log, probing ? MSGL_DEBUG : MSGL_ERR,
                       "Cannot map unknown multi-plane format: 0x%08X\n",
                       p_mapper->desc.layers[i].format);
                return false;
            }
        } else {
            /*
             * As OpenGL only has one guaranteed rgba format (rgba8), drivers
             * that support importing dmabuf formats with different channel
             * orders do implicit swizzling to get to rgba. However, we look at
             * the original imgfmt to decide channel order, and we then swizzle
             * based on that. So, we can get into a situation where we swizzle
             * twice and end up with a mess.
             *
             * The simplest way to avoid that is to lie to OpenGL and say that
             * the surface we are importing is in the natural channel order, so
             * that our swizzling does the right thing.
             *
             * DRM ABGR corresponds to OpenGL RGBA due to different naming
             * conventions.
             */
            switch (format[0]) {
            case DRM_FORMAT_ARGB8888:
            case DRM_FORMAT_RGBA8888:
            case DRM_FORMAT_BGRA8888:
                format[0] = DRM_FORMAT_ABGR8888;
                break;
            case DRM_FORMAT_XRGB8888:
                format[0] = DRM_FORMAT_XBGR8888;
                break;
            case DRM_FORMAT_RGBX8888:
            case DRM_FORMAT_BGRX8888:
                // Logically, these two formats should be handled as above,
                // but there appear to be additional problems that make the
                // format change here insufficient or incorrect, so we're
                // doing nothing for now.
                break;
            }
        }

        for (int j = 0; j < p_mapper->desc.layers[i].nb_planes; j++, n++) {
            int attribs[48] = {EGL_NONE};
            int num_attribs = 0;

            ADD_ATTRIB(EGL_LINUX_DRM_FOURCC_EXT, format[j]);
            ADD_ATTRIB(EGL_WIDTH,  p_mapper->tex[n]->params.w);
	    //printf("width: %d\n", p_mapper->tex[n]->params.w); //debug
            ADD_ATTRIB(EGL_HEIGHT, p_mapper->tex[n]->params.h);
	    //printf("height: %d\n", p_mapper->tex[n]->params.h); //debug
	    /*for (int bpp = 0; bpp < 257; bpp++) {
	    	printf("result: %d\n", check_modifier(p, format[j], p_mapper->desc.objects[p_mapper->desc.layers[i].planes[j].object_index].format_modifier, p_mapper->tex[n]->params.w*bpp));
	    }*/
            //p_mapper->desc.layers[i].planes[j].offset = 0; //maybe?
            ADD_PLANE_ATTRIBS(0);
	    //printf("result: %d\n", check_modifier(p, format[j], p_mapper->desc.objects[p_mapper->desc.layers[i].planes[j].object_index].format_modifier, 0));
	    //unsigned int bpp = p_mapper->layout.fmt.bpp[j]; //maybe?
	    //printf("bpp: %d\n", bpp); //debug
    	    //printf("format: %s\n", drm_string(format[j], 0)); //debug
            //printf("offset: %ld\n", p_mapper->desc.layers[i].planes[j].offset); //debug
            //printf("pitch: %ld\n", p_mapper->desc.layers[i].planes[j].pitch); //debug
	    //printf("%#010lx\n", p_mapper->desc.objects[p_mapper->desc.layers[i].planes[j].object_index].format_modifier); //debug
	    //printf("\n"); //debug

            p->images[n] = p->CreateImageKHR(eglGetCurrentDisplay(),
                EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)NULL, attribs);
            if (!p->images[n]) {
                mp_msg(mapper->log, probing ? MSGL_DEBUG : MSGL_ERR,
                    "Failed to import surface in EGL: %s\n", eglErrorString(eglGetError()));
                return false;
            }

            gl->BindTexture(GL_TEXTURE_2D, p->gl_textures[n]);
            p->EGLImageTargetTexture2DOES(GL_TEXTURE_2D, p->images[n]);

            mapper->tex[n] = p_mapper->tex[n];
        }
    }

    gl->BindTexture(GL_TEXTURE_2D, 0);
    return true;
}

static void vaapi_gl_unmap(struct ra_hwdec_mapper *mapper)
{
    struct dmabuf_interop_priv *p_mapper = mapper->priv;
    struct vaapi_gl_mapper_priv *p = p_mapper->interop_mapper_priv;

    if (p) {
        for (int n = 0; n < 4; n++) {
            if (p->images[n])
                p->DestroyImageKHR(eglGetCurrentDisplay(), p->images[n]);
            p->images[n] = 0;
        }
    }
}

bool dmabuf_interop_gl_init(const struct ra_hwdec *hw,
                            struct dmabuf_interop *dmabuf_interop)
{
    if (!ra_is_gl(hw->ra)) {
        // This is not an OpenGL RA.
        return false;
    }

    if (!eglGetCurrentContext())
        return false;

    const char *exts = eglQueryString(eglGetCurrentDisplay(), EGL_EXTENSIONS);
    if (!exts)
        return false;

    GL *gl = ra_gl_get(hw->ra);
    if (!gl_check_extension(exts, "EGL_EXT_image_dma_buf_import") ||
        !gl_check_extension(exts, "EGL_KHR_image_base") ||
        !gl_check_extension(gl->extensions, "GL_OES_EGL_image") ||
        !(gl->mpgl_caps & MPGL_CAP_TEX_RG))
        return false;

    dmabuf_interop->use_modifiers =
        gl_check_extension(exts, "EGL_EXT_image_dma_buf_import_modifiers");

    MP_VERBOSE(hw, "using EGL dmabuf interop\n");

    dmabuf_interop->interop_init = vaapi_gl_mapper_init;
    dmabuf_interop->interop_uninit = vaapi_gl_mapper_uninit;
    dmabuf_interop->interop_map = vaapi_gl_map;
    dmabuf_interop->interop_unmap = vaapi_gl_unmap;

    return true;
}
