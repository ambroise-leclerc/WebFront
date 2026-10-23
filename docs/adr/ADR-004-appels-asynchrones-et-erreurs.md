# ADR-004 : Des appels asynchrones aux contrats délibérément asymétriques

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2022-02-06 | Acceptée | Ambroise Leclerc | Décision initiale : appels C++ vers JavaScript sans retour. | [4fd263e](https://github.com/ambroise-leclerc/WebFront/commit/4fd263e4e19598c9b81355ec0de80aa6dda8ecb3) |
| 1.1 | 2022-02-13 | Acceptée | Ambroise Leclerc | Enregistrement de callbacks C++ appelables depuis JavaScript. | [05d0a72](https://github.com/ambroise-leclerc/WebFront/commit/05d0a72ac6a4ccc714eb6d6563ad2dd8cad41c87) |
| 2.0 | 2026-07-20 | Acceptée | Ambroise Leclerc | Résultats asynchrones : futurs C++ et promesses JavaScript, propagation des erreurs. | [481078b](https://github.com/ambroise-leclerc/WebFront/commit/481078ba9a74abf2cd1e30e3dc461ab3a8153403) |
| 2.1 | 2026-07-22 | Acceptée | Ambroise Leclerc | Rejet de ready lors d’un échec initial de connexion. | [64ac000](https://github.com/ambroise-leclerc/WebFront/commit/64ac0006b67f4a3e99983a42a58b1345c0b4e3ab) |
| 2.2 | 2026-07-22 | Acceptée | Ambroise Leclerc | Déduction des signatures des callbacks ordinaires. | [c065a25](https://github.com/ambroise-leclerc/WebFront/commit/c065a25a476b91a6bf2f77214f32c955b779310c) |
| 2.3 | 2026-10-13 | Acceptée | Ambroise Leclerc | Contrat explicite de l’asymétrie et couverture séparée des échecs dans les deux directions. | [aa7457b](https://github.com/ambroise-leclerc/WebFront/commit/aa7457ba98b6cf8109838e98345cfcfaa52d82f0) |
| 2.4 | 2026-10-18 | Acceptée | Ambroise Leclerc | API de senders différée et annulation locale, en complément des futures. | [0657923](https://github.com/ambroise-leclerc/WebFront/commit/0657923697ef697019f2a898754740971e79a79f) |

## État et chronologie

**Acceptée.** Décision initiale : **2022-02-06** (version 1.0).
Dernière révision : **2026-10-18** (version 2.4).
Le présent texte expose la décision en vigueur ; le tableau retrace ses états antérieurs.

## Contexte

Les premiers appels de C++ vers JavaScript ignoraient le résultat. Une révision de juillet
2026 introduisit les réponses asynchrones ; en octobre, la documentation cessa de présenter
tous les appels comme dépourvus de retour. « Asynchrone » ne dit rien, à lui seul, des
obligations de l’appelant.

## Décision

Que chaque direction et chaque forme d’API conserve son contrat propre :

| API | Contrat |
| --- | --- |
| `webFront.cppFunction(name)(...)` | Toujours une Promise, même pour un callback C++ `void` ; traiter son rejet. |
| `ui.jsFunction(name)(...)` | Envoi sans attente de résultat, seul appel C++ fire-and-forget. |
| `ui.jsFunction<R>(name)(...)` | Appel immédiat renvoyant un `std::future<R>`. |
| `ui.jsFunctionSender<R>(name)(...)` | Sender différé, envoi au démarrage, annulation locale possible. |
| `webFront.ready` | Résolution après l’accusé du handshake WebSocket ; rejet si la connexion initiale échoue ou ferme. |

Fonctions absentes, exceptions de callback, réponses malformées et déconnexions
font échouer le résultat attendu : `future.get()` lève, la Promise rejette, le sender termine en erreur.
Les callbacks enregistrés sont possédés par le stockage de l’application. Les signatures
ordinaires sont déduites ; les lambdas génériques et opérateurs surchargés utilisent
`cppFunction<R, Args...>` explicitement.

## Options écartées et motifs

- Rendre tous les appels sans retour : abandonnerait les résultats et erreurs intégrés.
- Bloquer le serveur pour simuler un appel synchrone : peut empêcher la réponse d’être traitée.
- Unifier futures, Promises et senders sous une promesse d’annulation distante : cette garantie
  n’existe pas ; seule l’attente locale des senders est annulable (ADR-006).

## Conséquences

- **Bénéfice :** erreurs et résultats observables sans imposer un seul style C++.
- **Coût accepté :** plusieurs formes d’API et gestion obligatoire des rejets JavaScript.
- **Risque :** attente sans fin d’une fonction qui ne répond pas ; il n’existe ni délai de réponse
  automatique ni accusé de livraison par écriture. Une déconnexion termine les appels pendants.

## Prescriptions d’implémentation et de génération de code

- Dans les exemples et tests JS, que toute Promise soit attendue ou interceptée, y compris les appels
  de fonctions C++ `void` et l’échec initial de `ready`.
- Qu’aucune valeur de retour ne soit attribuée à l’appel C++ non typé.
- Que `future.get()` ne bloque pas le thread serveur ; qu’une coroutine prenne en charge toute
  attente qui doive s’y dérouler.
- Que les fonctions enregistrées soient possédées ; qu’aucune référence à un paramètre de
  transfert ne survive à l’enregistrement.
- Tester séparément les échecs C++→JS et JS→C++, sans confondre les deux contrats.

## Sources et vérification

- [API C++](../../include/webfront/WebFront.hpp), [fonctions JS](../../include/webfront/JsFunction.hpp),
  [exécution asynchrone](../ASYNC_EXECUTION.md).
- Tests : [JSFunctionTests](../../test/unit/JSFunctionTests.cpp), [WebLinkTests](../../test/unit/WebLinkTests.cpp),
  [webfront-specs](../../test/web/suite/specs/webfront-specs.mjs).
- Voir [ADR-003](ADR-003-protocole-binaire-et-bornes.md) et [ADR-006](ADR-006-taches-structurees-et-arret.md).
