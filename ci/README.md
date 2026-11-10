# Build ffmpeg (configuration Plex)

Construit un binaire `ffmpeg` **statique unique** par plateforme, avec la
configuration du Plex Transcoder (Plex Media Server 1.43.5.11029,
`Plex Transcoder -buildconf`).

| Cible            | Runner GitHub          | Méthode                                   |
|------------------|------------------------|-------------------------------------------|
| `linux-x86_64`   | `ubuntu-24.04`         | conteneur `ubuntu:22.04`, clang           |
| `linux-arm64`    | `ubuntu-24.04-arm`     | conteneur `ubuntu:22.04`, clang           |
| `windows-x86_64` | `ubuntu-24.04`         | compilation croisée llvm-mingw (comme Plex) |
| `macos-arm64`    | `macos-15`             | clang Xcode, macOS 11.0 minimum           |
| `macos-x86_64`   | `macos-15-intel`       | clang Xcode, macOS 10.13 minimum          |
| `macos-universal`| `macos-15`             | `lipo` des deux binaires macOS, dans le job `release` |

Workflow : `.github/workflows/build-ffmpeg.yml`. Il se déclenche sur chaque push
de branche, les pull requests et manuellement (`workflow_dispatch`). Chaque
push sur `master` publie en plus une release GitHub `v<version>-<n° de run>`
(par ex. `v8.1.3-12`) avec les binaires de toutes les plateformes.

## Fichiers

- `config/common.txt` : options `configure` communes, reprises de Plex.
- `config/codecs.txt` : codecs que Plex télécharge à part (« codec packs »)
  et qui sont ici intégrés au binaire. **Liste reconstituée**, Plex ne publie
  pas la sienne : à ajuster selon vos besoins.
- `config/<cible>.txt`, `config/macos.txt` : options matérielles de Plex par
  plateforme (VAAPI/NVENC/CUDA/OpenCL, QSV/MediaFoundation/D3D11, OMX,
  VideoToolbox/AudioToolbox) et hwaccels des décodeurs intégrés.
- `deps.sh` : compile les bibliothèques tierces en statique (zlib, freetype,
  fribidi, harfbuzz, libass, ogg, vorbis, opus, dav1d, libxml2, openssl, lame,
  x264, + selon la cible nv-codec-headers, chargeur OpenCL Khronos, libdrm,
  libva, libvpl, en-têtes OpenMAX).
- `build.sh <cible>` : chaîne de compilation, dépendances (mises en cache),
  configure avec les mêmes options de compilation que Plex (clang, `-O3`,
  ThinLTO, `-ffunction-sections`/`--gc-sections`, `-g2`), puis compilation.
- `setup-linux.sh` : outils de build pour les conteneurs Ubuntu 22.04.

## Différences avec le build Plex

- Statique (`--enable-static --disable-shared`) au lieu de bibliothèques
  partagées, et pas de chargement de codecs externes (`--external-decoder`).
- Les codecs des packs Plex sont intégrés, avec leurs hwaccels, ce qui ajoute
  `--enable-gpl` (libx264) et `--enable-version3` (requis par FFmpeg 8.x avec
  OpenSSL 3).
- Linux : bibliothèques tierces statiques mais glibc dynamique (glibc ≥ 2.35),
  pour que les pilotes CUDA/NVENC, VAAPI et OpenCL puissent être chargés à
  l'exécution. Plex utilise musl avec ses propres bibliothèques partagées.
- Le binaire publié est strippé ; la version avec symboles est dans
  l'artefact `*-debug` (Plex utilise `--strip=true`).
- `ayuv` : décodeur retiré de FFmpeg en 7.0.

## Build local

```sh
# Linux / Windows, dans Docker
docker run --rm -it -v "$PWD:/src" -w /src ubuntu:22.04 \
    bash -c 'ci/setup-linux.sh windows && ci/build.sh windows-x86_64'

# macOS
brew install meson ninja nasm cmake pkg-config autoconf automake libtool
ci/build.sh macos-arm64
```

Résultat : `dist/<cible>/ffmpeg[.exe]` et `dist/<cible>/debug/`.
