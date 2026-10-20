# ADR-005 : D’ASIO standalone à stdnet : anticiper le réseau standard sans bloquer son évolution

## Versions

| Version | Date | État | Auteur | Évolution | Commit |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2022-01-21 | Acceptée | Ambroise Leclerc | Création initiale : HTTP et WebSocket sur la lignée ASIO/Networking TS. | [a8bce8c](https://github.com/ambroise-leclerc/WebFront/commit/a8bce8c96767b9c8c75fbaf00a6234121505db34) |
| 1.1 | 2023-02-12 | Acceptée | Ambroise Leclerc | Provider réseau et mocks pour tester indépendamment des sockets réelles. | [e1c2b5c](https://github.com/ambroise-leclerc/WebFront/commit/e1c2b5c846c4da6a19de7a5ed758cf0a1dfe8195) |
| 1.2 | 2023-05-04 | Acceptée | Ambroise Leclerc | Dépendance Networking TS portée par la cible de bibliothèque. | [8be2994](https://github.com/ambroise-leclerc/WebFront/commit/8be29945fbb390b1db6a8f6f00e1348b698be229) |
| 2.0 | 2026-10-13 | Acceptée | Ambroise Leclerc | Remplacement du backend Networking TS par stdnet. | [0e8bbc0](https://github.com/ambroise-leclerc/WebFront/commit/0e8bbc0d6035c3d25b9488fb9ed4c9b0d7c08d32) |
| 2.1 | 2026-10-13 | Acceptée | Ambroise Leclerc | Fermeture de l’acceptor et des connexions sur le thread du contexte. | [ad6f0a7](https://github.com/ambroise-leclerc/WebFront/commit/ad6f0a7863b12fb35e0851be5648539618231190) |
| 3.0 | 2026-10-13 | Acceptée | Ambroise Leclerc | HTTP et WebSocket convertis en coroutines attendant les senders réseau. | [1e765ca](https://github.com/ambroise-leclerc/WebFront/commit/1e765ca4275e4cfb9b4e7d68b204ce7d9261582c) |
| 3.1 | 2026-10-16 | Acceptée | Ambroise Leclerc | Centralisation des en-têtes sender avant les macros CEF. | [ca9c35d](https://github.com/ambroise-leclerc/WebFront/commit/ca9c35d2c7a0acb2bc426d4a932cc04b21470a7b) |

## Statut et date

**Acceptée.** Création initiale : **2022-01-21** (version 1.0).
Dernière évolution : **2026-10-16** (version 3.1).
Le corps de l’ADR décrit la décision dans sa dernière version ; les versions antérieures
et leurs changements sont identifiés dans le tableau ci-dessus.

## Contexte

Le choix fondateur, vers 2020, consiste à implémenter HTTP et WebSocket au-dessus d’ASIO
standalone, sans dépendance à Boost, dans la perspective où sa lignée fournirait le réseau
de C++23. ASIO est la base de travail disponible ; C++23 est alors la cible anticipée,
pas une norme publiée fournissant déjà ces API. Cette intention est précisée par le fondateur.

Les premières versions conservées dans les branches historiques emploient déjà
`<experimental/net>` et `std::experimental::net` pour HTTP et WebSocket en janvier 2022.
Le code vendored puis `networking-ts-impl` matérialise cette filiation ASIO/Networking TS ;
il faut distinguer cette API expérimentale de l’API `asio::` standalone d’origine et d’un
réseau effectivement entré dans le standard. En 2023, la dépendance est explicitement liée
à la cible de bibliothèque WebFront.

Les reports successifs de la standardisation remettent en cause l’attente initiale. Le
fondateur situe le passage à stdnet dans le contexte du nouveau report du réseau à l’horizon
C++29. Le projet choisit alors de progresser avec un backend fondé sur les senders, plutôt
que d’attendre une nouvelle échéance. Cet horizon explique le choix de WebFront ; il ne
constitue pas une garantie d’adoption ou de compatibilité future par WG21.

En octobre 2026, stdnet remplace le provider Networking TS, puis HTTP/WebSocket sont convertis
en coroutines. Le flux réseau se rapproche de son ordre logique, avec des tests sans sockets
réelles. Le contrat actuel ci-dessous découle de cette migration ; il ne décrit pas
l’implémentation ASIO initiale.

## Décision

Le réseau reste une dépendance injectée. `networking::TCPStdnet` adapte les opérations
`Accept`, `Receive`, `Send` de stdnet ; les mocks fournissent le même contrat. Les boucles HTTP
et WebSocket sont des `networking::Task` qui attendent les senders du provider.

Utiliser le vocabulaire sender via `networking::ex = ::stdnet::ex`. Le choix de stdexec
aujourd’hui, puis éventuellement de `std::execution`, appartient à stdnet. C++23 reste le
standard du projet ; il n’existe pas ici de réseau fourni directement par la bibliothèque
standard C++.

Centraliser les en-têtes sender dans `networking/SenderHeaders.hpp`, inclus avant CEF : ses
macros peuvent casser les attributs des en-têtes stdexec. Les requêtes d’arrêt peuvent venir
d’un autre thread, mais les opérations réseau se dénouent sur le thread du contexte.

## Alternatives et arbitrage

- **ASIO standalone puis Networking TS : choix initial, aujourd’hui remplacé comme backend
  actif.** Il permettait de développer sans attendre le réseau C++23 envisagé. Les reports
  successifs et l’évolution du modèle asynchrone ont conduit au choix stdnet.
- **Attendre un réseau standard C++29 : non retenu pour poursuivre WebFront.** Une échéance
  de standardisation ne fournit ni implémentation immédiate ni garantie d’API pour le projet.
- **stdnet : choix actuel.** Il fournit des opérations réseau compatibles avec le modèle
  sender retenu sans faire passer ses API pour des fonctions déjà standardisées.
- Coder directement les sockets ou stdexec dans les couches HTTP et bridge : contournerait
  la frontière d’injection et couplerait les tests à une implémentation.
- Fermer les objets I/O depuis le thread demandant l’arrêt : contraire au modèle de durée de
  vie retenu et aux corrections de courses de fermeture.

## Conséquences

- **Bénéfice :** flux séquentiels lisibles et tests déterministes des transitions réseau.
- **Coût accepté :** dépendance à stdnet et au support sender de sa chaîne d’outils.
- **Risque :** durée de vie d’opérations suspendues et incompatibilités de macros ; joindre les
  scopes avant destruction et maintenir la couverture d’inclusion CEF.

## Règles d’implémentation et de génération de code

- Préserver la frontière du provider lors d’un changement de backend ; ne pas remettre ASIO,
  `std::experimental::net` ou `stdnet` directement dans le code applicatif ou le codec.
- Distinguer dans les commentaires l’intention historique C++23, le backend effectivement
  employé et les perspectives C++29 ; ne pas prétendre que stdnet est le futur standard.
- Utiliser le provider et ses senders pour les nouvelles opérations, sans ajouter une boucle
  réseau concurrente ou une seconde infrastructure de callbacks détachés.
- Ne pas appeler `close()` sur une socket depuis un callback d’arrêt exécuté ailleurs.
- Conserver les données utilisées par un sender jusqu’à sa terminaison.
- Utiliser les mocks pour échecs d’acceptation, annulations et lectures fragmentées ; conserver
  un test réel de destruction pour valider aussi le backend stdnet.

## Historique Git

- **2022-01-16** — [3067222](https://github.com/ambroise-leclerc/WebFront/commit/3067222bdba985b9f30a05b056d88735236a3787) : reprise visant déjà C++23, avec une implémentation vendored de la Networking TS.
- **2022-01-21** — [a8bce8c](https://github.com/ambroise-leclerc/WebFront/commit/a8bce8c96767b9c8c75fbaf00a6234121505db34) : HTTP et WebSocket utilisent `<experimental/net>` dans les en-têtes historiques.
- **2023-02-12** — [e1c2b5c](https://github.com/ambroise-leclerc/WebFront/commit/e1c2b5c846c4da6a19de7a5ed758cf0a1dfe8195) : support des mocks réseau et chaîne de compilation C++23.
- **2023-05-04** — [8be2994](https://github.com/ambroise-leclerc/WebFront/commit/8be29945fbb390b1db6a8f6f00e1348b698be229) : Networking TS portée par la cible de bibliothèque WebFront.
- **2025-08-16** — [bbe4913](https://github.com/ambroise-leclerc/WebFront/commit/bbe491304583a6ea451ea5a25dc25ac9fc09488e) : regroupement du provider Networking TS antérieur ; ne date pas le choix réseau initial.
- **2026-10-13** — [0e8bbc0](https://github.com/ambroise-leclerc/WebFront/commit/0e8bbc0d6035c3d25b9488fb9ed4c9b0d7c08d32) : remplacement de la Networking TS par stdnet.
- **2026-10-13** — [ad6f0a7](https://github.com/ambroise-leclerc/WebFront/commit/ad6f0a7863b12fb35e0851be5648539618231190) : fermetures sur le thread du contexte.
- **2026-10-13** — [1e765ca](https://github.com/ambroise-leclerc/WebFront/commit/1e765ca4275e4cfb9b4e7d68b204ce7d9261582c) : HTTP/WebSocket migrés vers des coroutines attendant des senders.
- **2026-10-14** — [8404119](https://github.com/ambroise-leclerc/WebFront/commit/84041199dcc6f09ba06aa24bf4aeb4d68c197f16) : reprise de l’acceptation après échec.
- **2026-10-16** — [ca9c35d](https://github.com/ambroise-leclerc/WebFront/commit/ca9c35d2c7a0acb2bc426d4a932cc04b21470a7b) : en-têtes sender inclus avant les macros CEF.

## Références et vérification

- Précisions du fondateur : choix ASIO standalone
  vers 2020, anticipation C++23 et motivation du passage à stdnet face aux reports vers C++29.
- [WG21 P1792R0 (2019)](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2019/p1792r0.html) :
  filiation du modèle d’opérations asynchrones Asio vers la Networking TS.
- [WG21 P2453R0 (2022)](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p2453r0.html) :
  résultats des consultations de 2021 sur les modèles asynchrones et le réseau.
- [Registre WG21 de 2026, P4100R1](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2026/) :
  proposition de réseau visant C++29 ; témoigne de l’horizon de travail, pas d’une adoption acquise.
- [Provider stdnet](../../include/networking/TCPNetworkingStdnet.hpp),
  [tâches](../../include/networking/Tasks.hpp), [en-têtes sender](../../include/networking/SenderHeaders.hpp).
- Tests : [StdnetShutdownTests](../../test/StdnetShutdownTests.cpp),
  [HTTPServerTests](../../test/HTTPServerTests.cpp), [CEFMacrosTests](../../test/CEFMacrosTests.cpp).
- Voir [ADR-006](ADR-006-taches-structurees-et-arret.md).
