# osu! MP3 Extractor

Petit outil Windows qui parcourt ton dossier `Songs` d'osu! et extrait
les MP3 de chaque beatmap, en les renommant proprement (artiste - titre)
à partir des métadonnées des fichiers `.osu`.

## Fonctionnalités
- Détection automatique du dossier `osu!/Songs`
- Choix manuel du dossier source / destination
- Extraction en arrière-plan (thread dédié)
- Interface GDI+ avec image animé et retours visuels (attente / traitement / terminé)
- Exécutable autonome : images et animations sont embarquées, aucun fichier externe requis

## Compilation
Nécessite MinGW-w64 (g++) ou MSVC, GDI+ et les libs Win32 standard.

\`\`\`bash
windres resources.rc -O coff -o resources.o
g++ osu_extract.cpp resources.o -o osu-mp3-extract.exe -lgdiplus -lgdi32 -lole32 -luuid -lshell32 -static -static-libgcc -static-libstdc++ -mwindows
\`\`\`
