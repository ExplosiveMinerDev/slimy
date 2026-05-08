#pragma once

#include <string>

namespace pe::platform {

enum class AutomaticUpdateResult {
    NotConfigured,       ///< URL de verification vide
    UnsupportedPlatform, ///< Pas Windows
    FetchFailed,         ///< Telechargement du fichier hub impossible
    BadManifest,         ///< Pas de lignes build=/url= valides
    UpToDate,            ///< build annonce <= kClientBuild
    DownloadFailed       ///< Echec telechargement applique (exe intact)
};

/// Telecharge `hubManifestCheckUrl` (meme format texte que `--manifest` serveur : build=N, url=...).
/// Si `build` > `kClientBuild`, appelle `downloadAndRestartFromUrl(url)` (sort du process si OK).
AutomaticUpdateResult tryAutomaticUpdate(const std::string& hubManifestCheckUrl);

/// Windows : télécharge depuis `urlUtf8` (http/https) puis applique la mise à jour.
///
/// **URL directe** : un `.exe` PE (ou fichier > 512 Ko commençant par MZ) → remplace le binaire
/// courant et relance le jeu (comportement historique).
///
/// **Manifeste texte** (fichier ≤ 512 Ko, UTF-8) commençant par une ligne contenant
/// `# slimy-manifest` : liste de fichiers relatifs au dossier du `.exe` :
/// ```
/// # slimy-manifest v1
/// file maps/default.sjmap https://cdn.example.com/default.sjmap
/// file slimyjourney.exe https://cdn.example.com/slimyjourney.exe
/// ```
/// Les chemins ne peuvent pas contenir `..`. Les fichiers hors exe sont copiés tout de suite ;
/// si le nom du fichier = celui de l’exe en cours, copie différée + redémarrage (script batch).
/// Si le manifeste ne liste que des données (pas l’exe), le jeu continue sans quitter.
void downloadAndRestartFromUrl(const std::string& urlUtf8);

} // namespace pe::platform
