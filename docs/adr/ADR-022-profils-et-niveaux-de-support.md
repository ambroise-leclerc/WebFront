# ADR-022 : Profils de compilateurs et niveaux de support

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.1 | 2026-11-11 | Acceptée | Ambroise Leclerc | Audit CMake, plancher commun 3.31 et Git des distributions LTS. | Présente modification (#527). |
| 1.0 | 2026-11-11 | Acceptée | Ambroise Leclerc | Plancher GCC 13 S2, profils et clause de sortie. | Présente modification (#525). |

## État

Acceptée le 2026-11-11 : option B de #525, choisie par le mainteneur.

## Contexte

« Un compilateur C++23 » ne définit pas une matrice qualifiée. GCC 13 compile les
interfaces et exemples actuels ; #525 rapporte une qualification sur Orin avec GCC
13.4, hors sanitizers. #548 fournit déjà le job Linux aarch64. Les faux positifs
libstdc++ sous `-Wnull-dereference` rendent coûteux un second engagement `-Werror`.
Les corrections de compatibilité de #526 sont distinctes du plancher de compilateur.

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

## Outils de construction, distributions LTS (#527)

Le plancher reste **CMake 3.31** pour le dépôt et les consommateurs
`add_subdirectory`/CPM, même sans exemples ni tests. Il s'agit d'un choix de
support commun, pas d'une commande 3.31 indispensable à la bibliothèque. Abaisser
le plancher consommateur à 3.28 demanderait une qualification séparée des
dépendances épinglées et des générateurs ; le présent audit statique ne la remplace
pas. Aucun second plancher n'est annoncé. L'adaptateur mddlog garde ses exigences
4.0–4.3 et sa qualification macOS 4.3.1, décrites dans son guide.

Audit des `CMakeLists.txt`, de `cmake/`, `js/core/*.cmake` et `test/cmake/`, comparé
aux notes de version officielles [3.29](https://cmake.org/cmake/help/v3.31/release/3.29.html),
[3.30](https://cmake.org/cmake/help/v3.31/release/3.30.html) et
[3.31](https://cmake.org/cmake/help/v3.31/release/3.31.html) :

| Chemin | Usages pertinents et fonctionnalités après 3.28 |
| --- | --- |
| Consommateur ordinaire : racine, frontends, `ModuleScan`, helpers publics | Pas d'usage identifié après 3.28. `CMP0155` et `CXX_SCAN_FOR_MODULES` datent de 3.28 ; `cmake_language(DEFER)` de 3.19. La lecture de CMP0155 avant le minimum exclut aussi un simple abaissement à 3.22. |
| Ressources embarquées : `js/core/*.cmake` | Pas d'usage identifié après 3.28 : `cmake_path` (3.20), lecture/écriture de fichiers, expressions régulières, génération de cibles et contrôles de fraîcheur. |
| Provisionnement et Jasmine optionnel : `WebFrontBootstrap`, `WebFrontRegistry`, `WebFrontJasmine` | Pas d'usage identifié après 3.28 : JSON (3.19), `file(COPY_FILE)` (3.21), `ARCHIVE_EXTRACT` (3.18), téléchargements avec `TLS_VERIFY` et délais. Pas de `TLS_VERSION` (3.30). |
| Contributeur : `DeveloperCompatibility.cmake`, `test/cmake/` et cibles de tests | Les minima explicites restent 3.31. Les opérations de table de compatibilité, archives, sous-configurations et `ctest --output-junit` n'imposent pas elles-mêmes une version après 3.28. Le test mddlog désactive `CXX_MODULE_STD` (3.30), en complément de la désactivation du scan. Les tests de réflexion GCC 16 sélectionnent C++26, dont la prise en charge complète arrive en 3.30. |
| Adaptateur mddlog activé | `CXX_MODULE_STD` (3.30), `CMAKE_CXX_COMPILER_IMPORT_STD` et portes expérimentales `import std` ; les contrôles de version imposent déjà CMake 4.x avant la découverte du compilateur. Hors du chemin header-only ordinaire. |

Ni `CODEGEN`, ni politique CMP016x/CMP017x explicite, ni expression génératrice
introduite après 3.28 n'a été identifiée dans ces chemins ordinaires. Les
politiques implicites sélectionnées par `cmake_minimum_required(3.31)` et le code
téléchargé de CPM/stdnet/stdexec et des moteurs restent aussi à qualifier avant
un éventuel abaissement ; cet inventaire ne promet pas leur compatibilité 3.28.

Ubuntu 22.04 fournit CMake 3.22.1 et Ubuntu 24.04 3.28.3 : installer un CMake tiers
est donc nécessaire. Le [README](../../README.md#build-tools-on-lts-distributions)
donne une installation isolée avec Python et un lien vers le dépôt APT Kitware.
La racine et `DeveloperCompatibility.cmake` indiquent ce chemin avant leur rejet
d'une version trop ancienne. La version livrée par un BSP Yocto doit être lue
dans son manifeste ; aucune version Scarthgap n'est déduite de cet audit.

Git 2.34.1 reste utilisable pour `NoAiAttributionRegression`. `--no-graph`, déjà
retiré par 808a095, ne protège d'aucune configuration qui active le graphe :
`log.graphColors` en définit seulement les couleurs. Le traitement des clés
`log.*` dans les sources Git [2.34.1](https://github.com/git/git/blob/v2.34.1/builtin/log.c)
et [courantes](https://github.com/git/git/blob/master/builtin/log.c), ainsi que la
[référence de configuration](https://git-scm.com/docs/git-config), ne fournit pas
de clé `log.graph`. Les alias ne remplacent pas la commande interne `log`.
Les options qui désactivent signatures, notes, décoration, patch et couleurs
restent nécessaires au cadrage des enregistrements et sont conservées ; le test
existant couvre les réglages utilisateur et les commits signés. On ne relève
donc pas le minimum Git pour une option défensive inutile.

## Alternatives et conséquences

L'option A laisse les régressions silencieuses. L'option C (GCC 14) exclut les
outillages GCC 13 alors que le code actuel compile. L'option D impose les coûts
des faux positifs et de tous les tiers à deux générations éloignées de GCC.
L'option B garde une voie pour les systèmes embarqués, au prix d'un gel partiel
des fonctionnalités publiques et d'un passage sanitizer supplémentaire en CI.
Les anciennes versions Clang ne reçoivent pas un minimum déduit des seules
fonctionnalités : leur qualification nécessite des tests à l'exécution avec une
bibliothèque standard identifiée. NDK et AppleClang restent des profils distincts.
