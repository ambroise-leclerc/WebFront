# ADR-011 : Confiner mddlog derrière une façade de diagnostics optionnelle

## Versions

| Version | Date | État | Auteur | Évolution | Commit |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-10-01 | Acceptée | Ambroise Leclerc | Création initiale : façade mddlog optionnelle et adaptateur compilé confiné. | [c886ff4](https://github.com/ambroise-leclerc/WebFront/commit/c886ff4ba469310448daa79b913436f961bc5992) |
| 1.1 | 2026-10-02 | Acceptée | Ambroise Leclerc | Transport navigateur via le consommateur borné de mddlog, hors des threads producteurs. | [f27be1c](https://github.com/ambroise-leclerc/WebFront/commit/f27be1c91ec8a4b7d49c03e243053e005c094632) |
| 1.2 | 2026-10-02 | Acceptée | Ambroise Leclerc | Limite des diagnostics en attente pour un navigateur lent. | [579c6f1](https://github.com/ambroise-leclerc/WebFront/commit/579c6f1c2a0019197b30d51a7ff56cfd65004690) |

## Statut et date

**Acceptée.** Création initiale : **2026-10-01** (version 1.0).
Dernière évolution : **2026-10-02** (version 1.2).
Le corps de l’ADR décrit la décision dans sa dernière version ; les versions antérieures
et leurs changements sont identifiés dans le tableau ci-dessus.

## Contexte

Le cœur dispose déjà d’un logger et peut transmettre ses diagnostics au navigateur. Une
intégration avec mddlog doit préserver les inclusions et le chemin header-only habituel,
sans répandre ses contraintes de modules C++ dans les consommateurs.

## Décision

`WEBFRONT_USE_MDDLOG=OFF` reste le défaut. `tooling/Logger.hpp` est l’entrée commune ; l’option
activée sélectionne la façade et lie l’adaptateur statique `WebFront_mddlog` (`WebFront::mddlog`).
`logging/Logger.cpp` est la seule unité consommatrice des modules. Les consommateurs incluent
des en-têtes et n’importent pas les modules mddlog.

L’option et les contraintes de toolchain sont cohérentes dans toutes les unités de traduction.
Le chemin activé utilise le package mddlog installé ou un checkout explicite, avec sa propre
matrice de compilateurs et de CMake, plus stricte que celle du cœur.

Avec mddlog, les logs navigateur passent par un transport borné, consommé hors des threads
producteurs. Avec ou sans lui, les diagnostics en attente pour un navigateur sont bornés ;
un transport en échec se détache sans entretenir une récursion de logs. Ce sont des diagnostics,
sans promesse de persistance d’audit ou de livraison garantie.

## Alternatives et arbitrage

- Rendre mddlog obligatoire ou importer ses modules dans les en-têtes publics : cela changerait
  le chemin d’intégration et la matrice du cœur.
- Garder un transport navigateur synchrone sur chaque producteur avec mddlog : remplacé par
  son consommateur borné.
- File illimitée pour un navigateur lent : risque mémoire, remplacé par des limites explicites.

## Conséquences

- **Bénéfice :** diagnostics enrichis sans coût obligatoire pour les applications simples.
- **Coût accepté :** deux configurations à vérifier et une exception compilée au header-only.
- **Risque :** champs empruntés ou callbacks utilisés après destruction ; copier les données
  nécessaires et détacher les registrations avant libération des propriétaires.
- **Risque :** saturation et perte de diagnostics ; exposer les refus sans annoncer une piste
  d’audit durable.

## Règles d’implémentation et de génération de code

- Inclure `tooling/Logger.hpp`, ne pas importer mddlog depuis les composants ou exemples.
- Sélectionner l’option via la cible CMake ; ne pas définir la macro différemment par fichier.
- Respecter le type opaque des handles du backend choisi ; ne pas supposer un indice entier.
- Garder l’exception compilée confinée à l’adaptateur. Ne pas étendre à tout le projet les
  contraintes de modules ou de runtime propres au chemin activé.
- Lors d’un changement de transport, vérifier les deux configurations, l’échec d’écriture,
  le détachement et le débordement sans récursion.

## Historique Git

- **2026-10-01** — [c886ff4](https://github.com/ambroise-leclerc/WebFront/commit/c886ff4ba469310448daa79b913436f961bc5992) : façade optionnelle mddlog et adaptateur compilé.
- **2026-10-02** — [f27be1c](https://github.com/ambroise-leclerc/WebFront/commit/f27be1c91ec8a4b7d49c03e243053e005c094632) : transport navigateur via le consommateur borné de mddlog.
- **2026-10-02** — [579c6f1](https://github.com/ambroise-leclerc/WebFront/commit/579c6f1c2a0019197b30d51a7ff56cfd65004690) : limite des diagnostics en attente côté navigateur.
- **2026-10-02** — [ca0e6fe](https://github.com/ambroise-leclerc/WebFront/commit/ca0e6fe389051edfc550a2fed7904cce72853413) : consignation de l’acceptation de l’ADR-003 mddlog après adoption.

## Références et vérification

- [Contrat et matrice mddlog](../mddlog-integration.md),
  [façade](../../include/tooling/Logger.hpp), [adaptateur](../../logging/Logger.cpp).
- Tests : [LoggerTests](../../test/LoggerTests.cpp), [MddlogLoggerTests](../../test/MddlogLoggerTests.cpp),
  [MddlogTransportTests](../../test/MddlogTransportTests.cpp).
- Voir [ADR-001](ADR-001-coeur-cpp23-header-only.md) et [ADR-006](ADR-006-taches-structurees-et-arret.md).
