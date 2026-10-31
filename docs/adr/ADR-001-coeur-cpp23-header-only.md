# ADR-001 : Un cœur C++23 en header-only, intégré à l’application

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2020-04-19 | Acceptée | Ambroise Leclerc | Décision initiale : application C++ et interface web, première base du projet. | [3acfe58](https://github.com/ambroise-leclerc/WebFront/commit/3acfe58b97dce207063c247ebc00906a4bbe749a) |
| 1.1 | 2022-01-16 | Acceptée | Ambroise Leclerc | Cible C++23 explicite dans CMake et reprise de l’architecture en en-têtes. | [3067222](https://github.com/ambroise-leclerc/WebFront/commit/3067222bdba985b9f30a05b056d88735236a3787) |
| 1.2 | 2023-05-04 | Acceptée | Ambroise Leclerc | Bibliothèque en en-têtes formalisée par une cible CMake INTERFACE. | [8be2994](https://github.com/ambroise-leclerc/WebFront/commit/8be29945fbb390b1db6a8f6f00e1348b698be229) |
| 1.3 | 2026-07-22 | Acceptée | Ambroise Leclerc | Façade par défaut : WebFrontConfig, adresse de bouclage et show(). | [24fce3a](https://github.com/ambroise-leclerc/WebFront/commit/24fce3a993a07d73925660da7a74c195184293cc) |
| 1.4 | 2026-10-13 | Acceptée | Ambroise Leclerc | Parcours débutant, exemples sans options et consommation comme sous-projet. | [674d554](https://github.com/ambroise-leclerc/WebFront/commit/674d554ef060ae824a59762c4da3f425aebf9742) |
| 1.5 | 2026-10-13 | Acceptée | Ambroise Leclerc | Couverture, assainisseurs et édition des liens confinés aux cibles internes. | [9b69ee5](https://github.com/ambroise-leclerc/WebFront/commit/9b69ee523e60f4ec476a1dae3a022e3ccaa85c24) |

## État et chronologie

**Acceptée.** Décision initiale : **2020-04-19** (version 1.0).
Dernière révision : **2026-10-13** (version 1.5).
Le présent texte expose la décision en vigueur ; le tableau retrace ses états antérieurs.

## Contexte

WebFront donne à l’application C++ une interface HTML, CSS et JavaScript sans lui prescrire
d’armature applicative. Une implémentation en C++23 était visée depuis 2020, dans l'espoir
qu’une bibliothèque réseau standard naquît de la lignée ASIO/Networking TS ; cette intention
précéda les traces Git conservées.

## Décision

Le cœur de production forme une bibliothèque C++23 en header-only sous `include/`, exposée par
la cible CMake `WebFront::WebFront`. `src/` contient les exemples, `test/` les tests actuels.
`BasicWF<NetProvider, Filesystem, Policy, Frontend>` compose les dépendances et possède le
serveur, les connexions, les callbacks et le thread serveur ; `BasicUI` représente un navigateur.

La portabilité repose sur trois frontières : le système de fichiers fournit les ressources
(ADR-002), le fournisseur réseau assure les entrées et sorties (ADR-005), et l’interface
d’affichage ouvre la page (ADR-007). Toute déclinaison embarquée ou mobile doit adapter ces
frontières sans réécrire le pont ni assujettir HTTP à son stockage.

L’API d’entrée reste `webfront::WebFront app; app.show();`. Par défaut, elle écoute sur
loopback, port 9002, sans racine de fichiers locaux. Des paramètres explicites permettent
la spécialisation. WebFront demeure expérimental ; aucune API générale de composants visuels
ou de DOM C++ n’est acquise.

Le projet parent conserve ses réglages globaux. Les exemples ne sont activés par défaut
qu’au niveau principal, les tests demandent `WEBFRONT_ENABLE_TESTING=ON`. L’adaptateur mddlog compilé
est une exception optionnelle documentée par l’ADR-011.

## Options écartées et motifs

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

## Prescriptions d’implémentation et de génération de code

- Que l’implémentation publique demeure dans des en-têtes conformes à l’ODR ; que les `.cpp`
  de `src/` servent aux exemples. Aucune bibliothèque compilée ne saurait devenir obligatoire.
- Préférer la façade simple dans un nouvel exemple ; n’exposer `BasicWF` que si une variation
  réelle de provider, filesystem, politique ou frontend le justifie.
- Respecter C++23, les concepts et conventions de `.github/CONTRIBUTING.md`. Ne pas confondre le
  vocabulaire sender fourni par stdnet avec une bibliothèque réseau standard déjà disponible.
- Ne pas imposer avertissements, sanitizers, couverture ou choix de linker au projet parent.

## Sources et vérification

- [API et composition](../../include/webfront/WebFront.hpp), [CMake](../../CMakeLists.txt),
  [conventions](../../.github/CONTRIBUTING.md), [état du projet](../PROJECT_STATUS.md).
- Vérifier `WebFrontTests.cpp`, `DefaultFSTests.cpp`, `AddSubdirectoryConsumer` et
  `NewcomerDocsMatchExamples` dans CTest.
- Voir [ADR-002](ADR-002-systemes-de-fichiers-composables.md),
  [ADR-005](ADR-005-evolution-reseau-asio-stdnet.md) et [ADR-007](ADR-007-frontends-explicites.md).
