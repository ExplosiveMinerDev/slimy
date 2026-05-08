#pragma once

#include <string>

namespace pe::platform {

/// Windows: télécharge l’exe depuis `urlUtf8` (http/https), remplace le binaire courant et relance.
/// Sort du processus en cas de succès. Non supporté sur les autres OS (no-op + message stderr).
void downloadAndRestartFromUrl(const std::string& urlUtf8);

} // namespace pe::platform
