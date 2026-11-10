/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_cuda_internal.h"
#include "libavutil/cuda_check.h"
#include "libavutil/mem.h"

#if CONFIG_SHADER_COMPRESSION
#include "libavutil/zlib_utils.h"
#endif

#include "load_helper.h"

#define CHECK_CU(x) FF_CUDA_CHECK_DL(avctx, cu, x)

/**
 * Decompress the embedded PTX if needed. On success *data and *length point
 * to the usable PTX, and *to_free must be freed by the caller with av_free().
 */
static int decompress_data(void *avctx, void **to_free, const void **data, unsigned int *length)
{
    *to_free = NULL;
#if CONFIG_SHADER_COMPRESSION
    {
        uint8_t *out;
        size_t out_len;
        int ret = ff_zlib_expand(avctx, &out, &out_len, *data, *length);
        if (ret < 0)
            return ret;
        *data = *to_free = out;
        *length = out_len;
    }
#endif
    return 0;
}

int ff_cuda_load_module(void *avctx, AVCUDADeviceContext *hwctx, CUmodule *cu_module,
                        const void *data, unsigned int length)
{
    CudaFunctions *cu = hwctx->internal->cuda_dl;
    void *to_free;
    int ret;

    if ((ret = decompress_data(avctx, &to_free, &data, &length)) < 0)
        return ret;

    ret = CHECK_CU(cu->cuModuleLoadData(cu_module, data));
    av_free(to_free);

    return ret;
}

//PLEX
int ff_cuda_link_add_data(void *avctx, AVCUDADeviceContext *hwctx, CUlinkState link_state,
                          const void *data, unsigned int length,
                          const char *name, unsigned int nb_options,
                          CUjit_option *options, void **option_values)
{
    CudaFunctions *cu = hwctx->internal->cuda_dl;
    void *to_free;
    int ret;

    if ((ret = decompress_data(avctx, &to_free, &data, &length)) < 0)
        return ret;

    ret = CHECK_CU(cu->cuLinkAddData(link_state, CU_JIT_INPUT_PTX, (void *)data, length, name,
                                     nb_options, options, option_values));
    av_free(to_free);

    return ret;
}
//PLEX
