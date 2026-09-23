#ifndef DFTRACER_UTILS_UTILITIES_HOST_OPS_H
#define DFTRACER_UTILS_UTILITIES_HOST_OPS_H

namespace dftracer::utils::utilities {

/**
 * @brief Register the utilities-layer host ops into the dataframe op registry.
 *
 * Adds the file, filesystem and text utilities a plugin reaches by name
 * through dftu.svc.ops: dftu.fs.scan_dir, dftu.fs.scan_dir_pattern,
 * dftu.file.compress, dftu.file.decompress and dftu.text.line_filter. Each is
 * synchronous and materializes its result, unlike the async utility it wraps.
 *
 * Idempotent and thread-safe. Both library variants run it from an
 * initializer, so a consumer never has to call it.
 */
void register_host_ops();

}  // namespace dftracer::utils::utilities

extern "C" {

/**
 * @brief Linker anchor for the host-op initializer; calls register_host_ops().
 *
 * An archive member is linked in only when something references it, so a
 * static build would otherwise drop the translation unit holding the
 * initializer and silently register no host op. The utilities static library
 * forces this symbol in through an interface link option, which is why the
 * name is unmangled. Not part of the API; call register_host_ops() instead.
 */
void dftu_register_host_ops(void);
}

#endif  // DFTRACER_UTILS_UTILITIES_HOST_OPS_H
