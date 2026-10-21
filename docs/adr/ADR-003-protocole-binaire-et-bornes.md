# ADR-003 : Un protocole binaire partagé, typé et borné

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2022-02-06 | Acceptée | Ambroise Leclerc | Décision initiale : encodage des appels C++ vers JavaScript. | [4fd263e](https://github.com/ambroise-leclerc/WebFront/commit/4fd263e4e19598c9b81355ec0de80aa6dda8ecb3) |
| 1.1 | 2022-02-13 | Acceptée | Ambroise Leclerc | Messages binaires et appels JavaScript vers C++. | [05d0a72](https://github.com/ambroise-leclerc/WebFront/commit/05d0a72ac6a4ccc714eb6d6563ad2dd8cad41c87) |
| 1.2 | 2022-02-22 | Acceptée | Ambroise Leclerc | Encodage et décodage des tuples. | [d88b55d](https://github.com/ambroise-leclerc/WebFront/commit/d88b55d43ab43ca029b7e8ed62f550abc8c6a7d7) |
| 1.3 | 2026-02-28 | Acceptée | Ambroise Leclerc | Politique configurable des tailles de tampons. | [0ff1363](https://github.com/ambroise-leclerc/WebFront/commit/0ff136346d2058c0136a7f06c41c111a9f6ac56e) |
| 1.4 | 2026-07-21 | Acceptée | Ambroise Leclerc | Tableaux numériques typés et contrôles de types et de bornes. | [6f10761](https://github.com/ambroise-leclerc/WebFront/commit/6f107614b021de24a27707566223d227a013d80d) |
| 1.5 | 2026-10-14 | Acceptée | Ambroise Leclerc | Rejet explicite des chaînes dépassant 65535 octets. | [d143858](https://github.com/ambroise-leclerc/WebFront/commit/d1438582a19dc3dc0975f190c7b745150a46860e) |
| 1.6 | 2026-10-16 | Acceptée | Ambroise Leclerc | Réassemblage des messages WebSocket fragmentés. | [3636c85](https://github.com/ambroise-leclerc/WebFront/commit/3636c8519b75bdfbcae8fcc8fca0c3a5f6374c1d) |
| 1.7 | 2026-10-16 | Acceptée | Ambroise Leclerc | Limite de message vérifiée dès l’en-tête, avant allocation de la charge utile. | [68f01f5](https://github.com/ambroise-leclerc/WebFront/commit/68f01f53f304ddb56155be0d47fb6fdd293de142) |
| 1.8 | 2026-10-16 | Acceptée | Ambroise Leclerc | Plafond indépendant de 125 octets pour les trames de contrôle. | [3ebc181](https://github.com/ambroise-leclerc/WebFront/commit/3ebc181fce48240aa2cecebfe4d3f8cc4966732d) |
| 1.9 | 2026-10-20 | Acceptée | Ambroise Leclerc | `std::bitset<N>` compacté dans le format Uint8Array existant, sans nouvel opcode. | [5bc294e](https://github.com/ambroise-leclerc/WebFront/commit/5bc294eb0a9ba81ae7789b425baa6e299e848d97) |

## État et chronologie

**Acceptée.** Décision initiale : **2022-02-06** (version 1.0).
Dernière révision : **2026-10-20** (version 1.9).
Le présent texte expose la décision en vigueur ; le tableau retrace ses états antérieurs.

## Contexte

Serveur et navigateur échangent appels, arguments et résultats sans dépendance à une
armature JavaScript. Leurs deux codecs doivent partager exactement codes d’opération,
longueurs et identifiants : des vérifications lacunaires ont jadis désynchronisé les
arguments et corrompu les messages.

## Décision

Que soit préservé le protocole binaire de `weblink/Messages.hpp` et de la source `src/WebFront.js`.

Note de renommage (0.4) : la source JavaScript est désormais `assets/WebFront.js` ; ce déplacement ne modifie pas le protocole décrit ici.
Les messages d’appel et de retour portent des identifiants de corrélation sur 16 bits.
Les types pris en charge sont les scalaires, chaînes, tuples, tableaux numériques typés et `std::bitset<N>` ;
les objets JavaScript arbitraires et objets DOM ne traversent pas automatiquement le pont.

La longueur des chaînes est bornée à 65 535 octets : C++ lève `std::length_error` à l’encodage et
JavaScript produit une `RangeError`. Un résultat de callback trop long rejette la Promise.
Les tableaux utilisent leur encodage propre et ne sont pas soumis à la limite des chaînes.

Les bitsets réutilisent les en-têtes unsigned-byte courts ou réguliers : exactement
`N / 8 + (N % 8 != 0)` octets, avec le bit C++ `i` dans le bit `i % 8` de l’octet
`i / 8` (poids faible en premier). Les bits hauts inutilisés sont nuls ; le décodeur rejette
un padding non nul, un autre type d’élément, une longueur différente ou un message tronqué.
`std::bitset<0>` est un `Uint8Array` vide. Le codec utilise l’interface publique du bitset,
sans dépendre de sa représentation, de l’endianness ou de la taille des mots hôtes.
JavaScript connaît `N` par le contrat applicatif : aucun nombre de bits n’est transmis.
Les contrats Promise/future/sender et leurs erreurs restent inchangés. L’encodage possède
ses octets compactés jusqu’à leur copie dans la trame ; la taille du bitset n’est pas bornée
par le tampon fixe des en-têtes. Le coût accepté est une allocation et un parcours bit à bit
par bitset, sans optimisation par mots à ce stade.

Le transport distingue trames WebSocket et messages : qu’il réassemble les messages
fragmentés, conserve les fragments incomplets et traite les trames réunies dans l’ordre.
La politique borne chaque message à 64 MiB par défaut ; que tout dépassement annoncé soit
refusé dès l’en-tête, fragments compris. Les trames de contrôle ont leur propre plafond de
125 octets.

## Options écartées et motifs

- JSON/RPC ou sérialisation automatique des objets : non retenus dans le protocole intégré ;
  ils changeraient son contrat de types et nécessiteraient une décision explicite.
- Tronquer silencieusement une longueur : comportement défectueux corrigé, à ne pas restaurer.
- Assimiler une réception réseau à une trame ou une trame à un message : incompatible avec
  les découpages et regroupements permis par WebSocket.

## Conséquences

- **Bénéfice :** contrat compact et tableaux numériques utilisables sans conversion en texte.
- **Coût accepté :** maintenance de deux codecs et limites visibles aux utilisateurs.
- **Risque :** dépassements, mauvais types ou allocation excessive ; contrôler les bornes avant
  lecture/allocation et exercer les décodeurs avec sanitizers et fuzzing.

## Prescriptions d’implémentation et de génération de code

- Que les codecs C++ et JavaScript évoluent de concert ; que `IndexFS.hpp` soit régénéré
  depuis les sources. Exception : un changement purement C++ qui réutilise un encodage
  existant, sans nouveau code de type ni modification du format binaire, peut laisser
  le codec JavaScript et `IndexFS.hpp` inchangés. Les tests doivent alors démontrer la
  compatibilité avec le codec JavaScript existant dans un navigateur réel.
- Que toute évolution du protocole s’accompagne de tests d’encodage et de décodage ainsi que
  d’une régression en navigateur réel, erreurs et frontières de taille comprises.
- Ne pas contourner une limite de chaîne par un cast de longueur ; découper les données ou
  employer un tableau numérique lorsque le contrat fonctionnel le permet.
- Garder `BuffersPolicyType` et ses valeurs de repli compatibles avec les politiques existantes.
- Rejeter les continuations mal placées et les trames de contrôle fragmentées ou trop longues.

## Sources et vérification

- [Codec C++](../../include/weblink/Messages.hpp), [codec JS](../../assets/WebFront.js),
  [politique de buffers](../../include/http/BuffersPolicy.hpp).
- Tests : [MessagesTests](../../test/MessagesTests.cpp), [WebSocketTests](../../test/WebSocketTests.cpp),
  [spécifications navigateur](../../webtest/js/webfront-specs.mjs).
- Voir [ADR-004](ADR-004-appels-asynchrones-et-erreurs.md) et [ADR-012](ADR-012-preuves-et-tests.md).
