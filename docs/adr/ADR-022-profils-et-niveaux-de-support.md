# ADR-022 : Profils de compilateurs et niveaux de support

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-11-11 | Acceptée | Ambroise Leclerc | Plancher GCC 13 S2, profils et clause de sortie. | Présente modification (#525). |

## État

Acceptée le 2026-11-11 : option B de #525, choisie par le mainteneur.

## Contexte

« Un compilateur C++23 » ne définit pas une matrice qualifiée. GCC 13 compile les
interfaces et exemples actuels ; #525 rapporte une qualification sur Orin avec GCC
13.4, hors sanitizers. #548 fournit déjà le job Linux aarch64. Les faux positifs
libstdc++ sous `-Wnull-dereference` rendent coûteux un second engagement `-Werror`.
Les corrections de compatibilité de #526 et le plancher CMake de #527 sont distincts.

## Décision

| Profil | Niveau | Engagement et limites |
| --- | --- | --- |
| GCC 16, Linux | S1, référence | Suite desktop à chaque PR, avertissements en erreurs ; tiers étendus selon TEST_TIERS. |
| GCC 13, Linux aarch64 | S2, best effort | Minimum déclaré pour les en-têtes et exemples ; compilation de toutes les cibles, tests rapides puis ASan/UBSan sur ces tests, chaque semaine et sur les PR vers master. Sans `-Werror`, sans navigateur. |
| Clang 23, Linux | S2 | Qualification des consommateurs, CEF et sanitizers dans les jobs étendus ; aucun plancher antérieur qualifié. |
| MSVC, AppleClang | S1 sur les images CI | Versions fournies par windows-2025 et macos-26 ; pas de promesse pour les versions antérieures. |

S1 décrit la validation de référence à chaque PR ; S2 une validation périodique
limitée à son profil. Un échec S2 rend le job rouge et demande une investigation ;
ce niveau ne promet ni tous les tiers, ni tous les avertissements en erreurs.
Les commandes et déclencheurs exacts sont dans [TEST_TIERS](../TEST_TIERS.md).
GCC 11 et 12 ne sont pas visés. Un compilateur et sa bibliothèque standard doivent
fournir notamment `<expected>` et `<format>`, les concepts et les coroutines.
Le minimum GCC ne s'applique pas à la réflexion C++26 (GCC 16) ni à mddlog
(GCC 16.1 exactement). Il ne qualifie pas à lui seul un BSP, CUDA ou un NDK.

Le plancher passe à GCC 14 dès qu'une des conditions suivantes est satisfaite :

- une fonctionnalité GCC 14 simplifie nettement un en-tête public ;
- un défaut de génération de code GCC 13 est démontré ;
- une dépendance épinglée exige un compilateur plus récent ;
- Scarthgap atteint le 30 avril 2028.

Ce relèvement est annoncé comme rupture dans le changelog, avec sa cause et les
profils CI mis à jour. Une montée de dépendance doit donc vérifier ce plancher.

## Éléments vérifiés et portée des preuves

- [stdexec en amont](https://github.com/NVIDIA/stdexec) annonce GCC 12 ; cela ne
  remplace pas la compilation de la révision épinglée par stdnet.
- [CUDA 13.2, tableau 3](https://docs.nvidia.com/cuda/archive/13.2.0/cuda-installation-guide-linux/index.html#host-compiler-support-policy)
  accepte GCC 6 à 15 sur x86_64 et Arm64 SBSA. La limite GCC 13 rapportée pour
  CUDA 12.6 ne doit donc pas être extrapolée à CUDA 13.2. Cela ne constitue pas
  une qualification WebFront avec nvcc ni de toutes les variantes JetPack.
- L'image [Ubuntu 24.04](https://github.com/actions/runner-images/blob/main/images/ubuntu/Ubuntu2404-Readme.md)
  est mutable : le job S2 impose `gcc-13`/`g++-13`, vérifie sa version et aarch64.
  Le job portability historique sans sélection explicite ne prouve pas à lui seul
  un plancher ; il imprime désormais le compilateur effectivement choisi.
- Aucun BSP NXP i.MX précis n'est sélectionné dans #525. Sa version GCC reste à
  vérifier dans le manifeste du BSP retenu ; aucune qualification i.MX n'est annoncée.

## Alternatives et conséquences

L'option A laisse les régressions silencieuses. L'option C (GCC 14) exclut les
outillages GCC 13 alors que le code actuel compile. L'option D impose les coûts
des faux positifs et de tous les tiers à deux générations éloignées de GCC.
L'option B garde une voie pour les systèmes embarqués, au prix d'un gel partiel
des fonctionnalités publiques et d'un passage sanitizer supplémentaire en CI.
Les anciennes versions Clang ne reçoivent pas un minimum déduit des seules
fonctionnalités : leur qualification nécessite des tests à l'exécution avec une
bibliothèque standard identifiée. NDK et AppleClang restent des profils distincts.
