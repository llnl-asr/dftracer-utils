#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/indexer/checkpoint.h>
#include <dftracer/utils/indexer/indexer.h>
#include <dftracer/utils/indexer/indexer_factory.h>

#include <cstring>

using namespace dftracer::utils;

extern "C" {

static int validate_handle(dft_indexer_handle_t indexer) {
    return indexer ? 0 : -1;
}

static std::shared_ptr<Indexer> *cast_indexer(dft_indexer_handle_t indexer) {
    return static_cast<std::shared_ptr<Indexer> *>(indexer);
}

dft_indexer_handle_t dft_indexer_create(const char *gz_path,
                                        const char *idx_path,
                                        uint64_t checkpoint_size,
                                        int force_rebuild) {
    if (!gz_path || !idx_path || checkpoint_size == 0) {
        DFTRACER_UTILS_LOG_ERROR("Invalid parameters for indexer creation", "");
        return nullptr;
    }

    try {
        auto indexer = IndexerFactory::create(
            gz_path, idx_path, checkpoint_size, force_rebuild != 0);
        if (indexer) {
            return static_cast<dft_indexer_handle_t>(
                new std::shared_ptr<Indexer>(indexer));
        }
        return nullptr;
    } catch (const std::exception &e) {
        DFTRACER_UTILS_LOG_ERROR("Failed to create DFT indexer: %s", e.what());
        return nullptr;
    }
}

int dft_indexer_build(dft_indexer_handle_t indexer) {
    if (validate_handle(indexer) < 0) {
        return -1;
    }

    try {
        (*cast_indexer(indexer))->build();
        return 0;
    } catch (const std::exception &e) {
        DFTRACER_UTILS_LOG_ERROR("Failed to build index: %s", e.what());
        return -1;
    }
}

int dft_indexer_need_rebuild(dft_indexer_handle_t indexer) {
    if (validate_handle(indexer)) {
        return -1;
    }

    try {
        return (*cast_indexer(indexer))->need_rebuild() ? 1 : 0;
    } catch (const std::exception &e) {
        DFTRACER_UTILS_LOG_ERROR("Failed to check if rebuild is needed: %s",
                                 e.what());
        return -1;
    }
}

int dft_indexer_exists(dft_indexer_handle_t indexer) {
    if (validate_handle(indexer) < 0) {
        return -1;
    }

    try {
        return (*cast_indexer(indexer))->exists() ? 1 : 0;
    } catch (const std::exception &e) {
        DFTRACER_UTILS_LOG_ERROR("Failed to check if index exists: %s",
                                 e.what());
        return -1;
    }
}

uint64_t dft_indexer_get_max_bytes(dft_indexer_handle_t indexer) {
    if (validate_handle(indexer) < 0) {
        return 0;
    }

    try {
        return (*cast_indexer(indexer))->get_max_bytes();
    } catch (const std::exception &e) {
        DFTRACER_UTILS_LOG_ERROR("Failed to get max bytes: %s", e.what());
        return 0;
    }
}

uint64_t dft_indexer_get_num_lines(dft_indexer_handle_t indexer) {
    if (validate_handle(indexer) < 0) {
        return 0;
    }

    try {
        return (*cast_indexer(indexer))->get_num_lines();
    } catch (const std::exception &e) {
        DFTRACER_UTILS_LOG_ERROR("Failed to get number of lines: %s", e.what());
        return 0;
    }
}

int dft_indexer_find_checkpoint(dft_indexer_handle_t indexer,
                                size_t target_offset,
                                dft_indexer_checkpoint_t *checkpoint) {
    if (validate_handle(indexer) < 0 || !checkpoint) {
        return -1;
    }

    try {
        IndexerCheckpoint temp_ckpt;

        if ((*cast_indexer(indexer))
                ->find_checkpoint(static_cast<size_t>(target_offset),
                                  temp_ckpt)) {
            checkpoint->uc_offset = static_cast<uint64_t>(temp_ckpt.uc_offset);
            checkpoint->c_offset = static_cast<uint64_t>(temp_ckpt.c_offset);
            checkpoint->bits = temp_ckpt.bits;
            checkpoint->dict_size = temp_ckpt.dict_compressed.size();

            checkpoint->dict_compressed =
                static_cast<unsigned char *>(malloc(checkpoint->dict_size));
            if (checkpoint->dict_compressed) {
                std::memcpy(checkpoint->dict_compressed,
                            temp_ckpt.dict_compressed.data(),
                            checkpoint->dict_size);
                return 1;
            }
            return -1;
        }
        return 0;
    } catch (const std::exception &e) {
        DFTRACER_UTILS_LOG_ERROR("Failed to find checkpoint: %s", e.what());
        return -1;
    }
}

int dft_indexer_get_checkpoints(dft_indexer_handle_t indexer,
                                dft_indexer_checkpoint_t **checkpoints,
                                size_t *count) {
    if (validate_handle(indexer) < 0) {
        return -1;
    }

    try {
        auto ckpts = (*cast_indexer(indexer))->get_checkpoints();

        auto temp_count = ckpts.size();
        if (temp_count == 0) {
            return 0;
        }

        auto temp_ckpts = static_cast<dft_indexer_checkpoint_t *>(
            malloc(temp_count * sizeof(dft_indexer_checkpoint_t)));
        if (!temp_ckpts) {
            return -1;
        }

        *checkpoints = temp_ckpts;

        // Initialize checkpoint information
        for (size_t i = 0; i < temp_count; i++) {
            const auto &checkpoint = ckpts[i];

            temp_ckpts[i].checkpoint_idx =
                static_cast<std::uint64_t>(checkpoint.checkpoint_idx);
            temp_ckpts[i].uc_offset =
                static_cast<std::uint64_t>(checkpoint.uc_offset);
            temp_ckpts[i].uc_size =
                static_cast<std::uint64_t>(checkpoint.uc_size);
            temp_ckpts[i].c_offset =
                static_cast<std::uint64_t>(checkpoint.c_offset);
            temp_ckpts[i].c_size =
                static_cast<std::uint64_t>(checkpoint.c_size);
            temp_ckpts[i].bits = checkpoint.bits;
            temp_ckpts[i].dict_size = checkpoint.dict_compressed.size();
            temp_ckpts[i].num_lines =
                static_cast<std::uint64_t>(checkpoint.num_lines);

            // Allocate and copy dictionary data
            temp_ckpts[i].dict_compressed =
                static_cast<unsigned char *>(malloc(temp_ckpts[i].dict_size));
            if (temp_ckpts[i].dict_compressed) {
                std::memcpy(temp_ckpts[i].dict_compressed,
                            checkpoint.dict_compressed.data(),
                            temp_ckpts[i].dict_size);
            } else {
                // Clean up on allocation failure
                for (size_t j = 0; j < i; j++) {
                    free(temp_ckpts[j].dict_compressed);
                }
                free(temp_ckpts);
                DFTRACER_UTILS_LOG_ERROR(
                    "Failed to allocate memory for checkpoint dictionary data",
                    "");
                *checkpoints = nullptr;
                return -1;
            }
        }

        *count = temp_count;
        *checkpoints = temp_ckpts;
        return 0;
    } catch (const std::exception &e) {
        DFTRACER_UTILS_LOG_ERROR("Failed to get checkpoints: %s", e.what());
        return -1;
    }
}

void dft_indexer_free_checkpoint(dft_indexer_checkpoint_t *checkpoint) {
    if (checkpoint) {
        free(checkpoint->dict_compressed);
        free(checkpoint);
    }
}

void dft_indexer_free_checkpoints(dft_indexer_checkpoint_t *checkpoints,
                                  size_t count) {
    if (checkpoints) {
        for (size_t i = 0; i < count; i++) {
            free(checkpoints[i].dict_compressed);
        }
        free(checkpoints);
    }
}

void dft_indexer_destroy(dft_indexer_handle_t indexer) {
    if (indexer) {
        delete cast_indexer(indexer);
    }
}

}  // extern "C"
