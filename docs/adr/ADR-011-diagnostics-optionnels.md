# ADR-011 : Confiner mddlog derrière une façade de diagnostic facultative

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-10-01 | Acceptée | Ambroise Leclerc | Décision initiale : façade mddlog optionnelle et adaptateur compilé confiné. | [c886ff4](https://github.com/ambroise-leclerc/WebFront/commit/c886ff4ba469310448daa79b913436f961bc5992) |
| 1.1 | 2026-10-02 | Acceptée | Ambroise Leclerc | Transport navigateur via le consommateur borné de mddlog, hors des threads producteurs. | [f27be1c](https://github.com/ambroise-leclerc/WebFront/commit/f27be1c91ec8a4b7d49c03e243053e005c094632) |
| 1.2 | 2026-10-02 | Acceptée | Ambroise Leclerc | Limite des diagnostics en attente pour un navigateur lent. | [579c6f1](https://github.com/ambroise-leclerc/WebFront/commit/579c6f1c2a0019197b30d51a7ff56cfd65004690) |

## État et chronologie

**Acceptée.** Décision initiale : **2026-10-01** (version 1.0).
Dernière révision : **2026-10-02** (version 1.2).
Le présent texte expose la décision en vigueur ; le tableau retrace ses états antérieurs.

## Contexte

Le cœur possède déjà un journal et peut transmettre ses diagnostics au navigateur. Pour
intégrer mddlog, il faut que les inclusions et le parcours usuel en en-têtes subsistent,
sans que ses contraintes de modules C++ gagnent les applications clientes.

## Décision

Que `WEBFRONT_USE_MDDLOG=OFF` demeure le réglage par défaut. `tooling/Logger.hpp` est l’entrée
commune ; l’option activée sélectionne la façade et lie l’adaptateur statique `WebFront_mddlog`
(`WebFront::mddlog`).
`cmake/mddlog/Logger.cpp` est la seule unité consommatrice des modules. Les consommateurs incluent
des en-têtes et n’importent pas les modules mddlog.

L’option et les contraintes de toolchain sont cohérentes dans toutes les unités de traduction.
Le chemin activé utilise le package mddlog installé ou un checkout explicite, avec sa propre
matrice de compilateurs et de CMake, plus stricte que celle du cœur.

Avec mddlog, les journaux destinés au navigateur empruntent un transport borné, consommé hors
des threads producteurs. Avec ou sans lui, les diagnostics en attente pour un navigateur sont bornés ;
un transport en échec se détache sans entretenir une récursion de logs. Ce sont des diagnostics,
sans promesse de persistance d’audit ou de livraison garantie.

## Options écartées et motifs

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

## Prescriptions d’implémentation et de génération de code

- Que composants et exemples incluent `tooling/Logger.hpp`, sans importer mddlog.
- Que l’option soit choisie par la cible CMake et que la macro ne varie pas d’un fichier à l’autre.
- Respecter le type opaque des handles du backend choisi ; ne pas supposer un indice entier.
- Garder l’exception compilée confinée à l’adaptateur. Ne pas étendre à tout le projet les
  contraintes de modules ou de runtime propres au chemin activé.
- Lors d’un changement de transport, vérifier les deux configurations, l’échec d’écriture,
  le détachement et le débordement sans récursion.

## Sources et vérification

- [Contrat et matrice mddlog](../mddlog-integration.md),
  [façade](../../include/webfront/tooling/Logger.hpp), [adaptateur](../../cmake/mddlog/Logger.cpp).
- Tests : [LoggerTests](../../test/unit/LoggerTests.cpp), [MddlogLoggerTests](../../test/unit/MddlogLoggerTests.cpp),
  [MddlogTransportTests](../../test/unit/MddlogTransportTests.cpp).
- Voir [ADR-001](ADR-001-coeur-cpp23-header-only.md) et [ADR-006](ADR-006-taches-structurees-et-arret.md).
