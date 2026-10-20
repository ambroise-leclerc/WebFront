# ADR-001 : Un cœur C++23 header-only intégré à l’application

## Versions

| Version | Date | État | Auteur | Évolution | Commit |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2020-04-19 | Acceptée | Ambroise Leclerc | Création initiale : application C++ et interface web, première base du projet. | [3acfe58](https://github.com/ambroise-leclerc/WebFront/commit/3acfe58b97dce207063c247ebc00906a4bbe749a) |
| 1.1 | 2022-01-16 | Acceptée | Ambroise Leclerc | Cible C++23 explicite dans CMake et reprise de l’architecture en en-têtes. | [3067222](https://github.com/ambroise-leclerc/WebFront/commit/3067222bdba985b9f30a05b056d88735236a3787) |
| 1.2 | 2023-05-04 | Acceptée | Ambroise Leclerc | Bibliothèque header-only formalisée par une cible CMake INTERFACE. | [8be2994](https://github.com/ambroise-leclerc/WebFront/commit/8be29945fbb390b1db6a8f6f00e1348b698be229) |
| 1.3 | 2026-07-22 | Acceptée | Ambroise Leclerc | Façade par défaut : WebFrontConfig, loopback et show(). | [24fce3a](https://github.com/ambroise-leclerc/WebFront/commit/24fce3a993a07d73925660da7a74c195184293cc) |
| 1.4 | 2026-10-13 | Acceptée | Ambroise Leclerc | Parcours débutant, exemples sans options et consommation comme sous-projet. | [674d554](https://github.com/ambroise-leclerc/WebFront/commit/674d554ef060ae824a59762c4da3f425aebf9742) |
| 1.5 | 2026-10-13 | Acceptée | Ambroise Leclerc | Options de couverture, sanitizers et linker confinées aux cibles internes. | [9b69ee5](https://github.com/ambroise-leclerc/WebFront/commit/9b69ee523e60f4ec476a1dae3a022e3ccaa85c24) |

## Statut et date

**Acceptée.** Création initiale : **2020-04-19** (version 1.0).
Dernière évolution : **2026-10-13** (version 1.5).
Le corps de l’ADR décrit la décision dans sa dernière version ; les versions antérieures
et leurs changements sont identifiés dans le tableau ci-dessus.

## Contexte

WebFront est conçu pour apporter une interface HTML/CSS/JavaScript à une application C++,
sans lui imposer un framework applicatif. Vers 2020, C++23 est déjà la cible visée, notamment
dans l’attente d’un réseau standard issu de la lignée ASIO/Networking TS. Cette intention,
précisée par le fondateur, précède sa matérialisation dans les commits retrouvés.

L’historique complet (`git log --all`) conserve la reprise de janvier 2022, dont le CMake
exige déjà `cxx_std_23`, malgré le titre « C++20 rework initial commit ». L’organisation
en en-têtes et la cible de bibliothèque se développent avant le regroupement de 2025.
L’historique du seul HEAD masque donc plusieurs jalons fondateurs. Les répertoires anciens
`webfront/` et `tests/` restent des archives, pas les points d’extension actuels.

## Décision

Le cœur de production est une bibliothèque C++23 header-only sous `include/`, exposée par
la cible CMake `WebFront::WebFront`. `src/` contient les exemples, `test/` les tests actuels.
`BasicWF<NetProvider, Filesystem, Policy, Frontend>` compose les dépendances et possède le
serveur, les connexions, les callbacks et le thread serveur ; `BasicUI` représente un navigateur.

La portabilité s’appuie sur des frontières distinctes : le filesystem choisit d’où viennent
les ressources (ADR-002), le provider réseau fournit les opérations I/O (ADR-005) et le
frontend décide comment afficher la page (ADR-007). Une déclinaison embarquée ou mobile doit
adapter ces frontières sans réécrire le bridge ni imposer son stockage au serveur HTTP.

L’API d’entrée reste `webfront::WebFront app; app.show();`. Par défaut, elle écoute sur
loopback, port 9002, sans racine de fichiers locaux. Des paramètres explicites permettent
la spécialisation. WebFront demeure une fondation expérimentale ; un modèle général de
widgets ou de DOM C++ n’est pas une API acquise.

Le projet parent conserve ses réglages globaux. Les exemples ne sont activés par défaut
qu’au niveau principal, les tests demandent `ENABLE_TESTING=ON`. L’adaptateur mddlog compilé
est une exception optionnelle documentée par l’ADR-011.

## Alternatives et arbitrage

- Poursuivre le prototype DOM de 2020 : il ne correspond pas à l’API active et ne doit pas
  être réintroduit implicitement.
- Imposer un framework applicatif, une classe de base ou un générateur de métadonnées : cela
  changerait le modèle d’intégration par callbacks ordinaires.
- Transformer tout le projet en modules C++ : cette voie n’est pas celle du cœur actuel ;
  les modules de mddlog restent confinés dans son adaptateur.

## Conséquences

- **Bénéfice :** intégration légère, composition explicite et exemples sans chaîne JavaScript.
- **Coût accepté :** instanciations de templates et dépendances visibles à la compilation ;
  header-only ne signifie ni absence de dépendances ni stabilité d’ABI.
- **Risque :** fuite d’options de compilation vers un parent ou violation ODR ; limiter les
  réglages aux cibles internes et vérifier un consommateur `add_subdirectory`.

## Règles d’implémentation et de génération de code

- Mettre l’implémentation publique dans des en-têtes ODR-safe ; réserver les `.cpp` de `src/`
  aux exemples. Ne pas inventer une bibliothèque compilée obligatoire.
- Préférer la façade simple dans un nouvel exemple ; n’exposer `BasicWF` que si une variation
  réelle de provider, filesystem, politique ou frontend le justifie.
- Respecter C++23, les concepts et conventions de `CONTRIBUTING.md`. Ne pas confondre le
  vocabulaire sender fourni par stdnet avec une bibliothèque réseau standard déjà disponible.
- Ne pas imposer avertissements, sanitizers, couverture ou choix de linker au projet parent.

## Références et vérification

- [API et composition](../../include/WebFront.hpp), [CMake](../../CMakeLists.txt),
  [conventions](../../CONTRIBUTING.md), [état du projet](../PROJECT_STATUS.md).
- Vérifier `WebFrontTests.cpp`, `DefaultFSTests.cpp`, `AddSubdirectoryConsumer` et
  `NewcomerDocsMatchExamples` dans CTest.
- Voir [ADR-002](ADR-002-systemes-de-fichiers-composables.md),
  [ADR-005](ADR-005-evolution-reseau-asio-stdnet.md) et [ADR-007](ADR-007-frontends-explicites.md).
