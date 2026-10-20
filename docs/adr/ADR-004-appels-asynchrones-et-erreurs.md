# ADR-004 : Des appels asynchrones aux contrats volontairement asymétriques

## Versions

| Version | Date | État | Auteur | Évolution | Commit |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2022-02-06 | Acceptée | Ambroise Leclerc | Création initiale : appels C++ vers JavaScript sans retour. | [4fd263e](https://github.com/ambroise-leclerc/WebFront/commit/4fd263e4e19598c9b81355ec0de80aa6dda8ecb3) |
| 1.1 | 2022-02-13 | Acceptée | Ambroise Leclerc | Enregistrement de callbacks C++ appelables depuis JavaScript. | [05d0a72](https://github.com/ambroise-leclerc/WebFront/commit/05d0a72ac6a4ccc714eb6d6563ad2dd8cad41c87) |
| 2.0 | 2026-07-20 | Acceptée | Ambroise Leclerc | Résultats asynchrones : futures C++ et Promises JavaScript, propagation des erreurs. | [481078b](https://github.com/ambroise-leclerc/WebFront/commit/481078ba9a74abf2cd1e30e3dc461ab3a8153403) |
| 2.1 | 2026-07-22 | Acceptée | Ambroise Leclerc | Rejet de ready lors d’un échec initial de connexion. | [64ac000](https://github.com/ambroise-leclerc/WebFront/commit/64ac0006b67f4a3e99983a42a58b1345c0b4e3ab) |
| 2.2 | 2026-07-22 | Acceptée | Ambroise Leclerc | Déduction des signatures des callbacks ordinaires. | [c065a25](https://github.com/ambroise-leclerc/WebFront/commit/c065a25a476b91a6bf2f77214f32c955b779310c) |
| 2.3 | 2026-10-13 | Acceptée | Ambroise Leclerc | Contrat explicite de l’asymétrie et couverture séparée des échecs dans les deux directions. | [aa7457b](https://github.com/ambroise-leclerc/WebFront/commit/aa7457ba98b6cf8109838e98345cfcfaa52d82f0) |
| 2.4 | 2026-10-18 | Acceptée | Ambroise Leclerc | API sender différée et annulation locale, en complément des futures. | [0657923](https://github.com/ambroise-leclerc/WebFront/commit/0657923697ef697019f2a898754740971e79a79f) |

## Statut et date

**Acceptée.** Création initiale : **2022-02-06** (version 1.0).
Dernière évolution : **2026-10-18** (version 2.4).
Le corps de l’ADR décrit la décision dans sa dernière version ; les versions antérieures
et leurs changements sont identifiés dans le tableau ci-dessus.

## Contexte

Les premiers appels C++ vers JavaScript ignoraient les résultats. Le commit de juillet 2026
intègre les résultats asynchrones, puis la documentation est corrigée en octobre pour éviter
l’affirmation erronée que tous les appels seraient sans retour. Le mot « asynchrone » ne
suffit pas à décrire les obligations de chaque appelant.

## Décision

Conserver des contrats explicites selon la direction et la forme d’API :

| API | Contrat |
| --- | --- |
| `webFront.cppFunction(name)(...)` | Toujours une Promise, même pour un callback C++ `void` ; traiter son rejet. |
| `ui.jsFunction(name)(...)` | Envoi sans attente de résultat, seul appel C++ fire-and-forget. |
| `ui.jsFunction<R>(name)(...)` | Appel immédiat renvoyant un `std::future<R>`. |
| `ui.jsFunctionSender<R>(name)(...)` | Sender différé, envoi au démarrage, annulation locale possible. |
| `webFront.ready` | Résolution après l’accusé du handshake WebSocket ; rejet si la connexion initiale échoue ou ferme. |

Les fonctions absentes, exceptions de callback, retours malformés et déconnexions font échouer
le résultat attendu : `future.get()` lève, la Promise rejette, le sender termine en erreur.
Les callbacks enregistrés sont possédés par le stockage de l’application. Les signatures
ordinaires sont déduites ; les lambdas génériques et opérateurs surchargés utilisent
`cppFunction<R, Args...>` explicitement.

## Alternatives et arbitrage

- Rendre tous les appels sans retour : abandonnerait les résultats et erreurs intégrés.
- Bloquer le serveur pour simuler un appel synchrone : peut empêcher la réponse d’être traitée.
- Unifier futures, Promises et senders sous une promesse d’annulation distante : cette garantie
  n’existe pas ; seule l’attente locale des senders est annulable (ADR-006).

## Conséquences

- **Bénéfice :** erreurs et résultats observables sans imposer un seul style C++.
- **Coût accepté :** plusieurs formes d’API et gestion obligatoire des rejets JavaScript.
- **Risque :** attente sans fin d’une fonction qui ne répond pas ; il n’existe ni délai de réponse
  automatique ni accusé de livraison par écriture. Une déconnexion termine les appels pendants.

## Règles d’implémentation et de génération de code

- Dans les exemples et tests JS, attendre ou intercepter toute Promise, y compris les appels
  de fonctions C++ `void` et l’échec initial de `ready`.
- Ne jamais attribuer de valeur de retour à l’appel C++ non typé.
- Éviter `future.get()` sur le thread serveur ; utiliser une coroutine si l’attente doit y vivre.
- Capturer les callables enregistrés par valeur/possession ; ne jamais conserver par référence
  un paramètre forwarding-reference passé à l’enregistrement.
- Tester séparément les échecs C++→JS et JS→C++, sans confondre les deux contrats.

## Historique Git

- **2022-02-06** — [4fd263e](https://github.com/ambroise-leclerc/WebFront/commit/4fd263e4e19598c9b81355ec0de80aa6dda8ecb3) : forme initiale des appels C++ vers JavaScript ; elle ignore les résultats.
- **2022-02-13** — [05d0a72](https://github.com/ambroise-leclerc/WebFront/commit/05d0a72ac6a4ccc714eb6d6563ad2dd8cad41c87) : Enregistrement de callbacks C++ appelables depuis JavaScript.
- **2026-07-20** — [481078b](https://github.com/ambroise-leclerc/WebFront/commit/481078ba9a74abf2cd1e30e3dc461ab3a8153403) : Résultats asynchrones : futures C++ et Promises JavaScript, propagation des erreurs.
- **2026-07-21** — [6f10761](https://github.com/ambroise-leclerc/WebFront/commit/6f107614b021de24a27707566223d227a013d80d) : intégration des résultats asynchrones et tableaux typés dans la branche courante.
- **2026-07-22** — [64ac000](https://github.com/ambroise-leclerc/WebFront/commit/64ac0006b67f4a3e99983a42a58b1345c0b4e3ab) : rejet de `ready` sur échec initial de connexion.
- **2026-07-22** — [c065a25](https://github.com/ambroise-leclerc/WebFront/commit/c065a25a476b91a6bf2f77214f32c955b779310c) : déduction des signatures des callbacks.
- **2026-10-13** — [aa7457b](https://github.com/ambroise-leclerc/WebFront/commit/aa7457ba98b6cf8109838e98345cfcfaa52d82f0) : clarification explicite de l’asymétrie et des erreurs.
- **2026-10-18** — [0657923](https://github.com/ambroise-leclerc/WebFront/commit/0657923697ef697019f2a898754740971e79a79f) : forme sender annulable ajoutée aux futures.

## Références et vérification

- [API C++](../../include/WebFront.hpp), [fonctions JS](../../include/JsFunction.hpp),
  [exécution asynchrone](../ASYNC_EXECUTION.md).
- Tests : [JSFunctionTests](../../test/JSFunctionTests.cpp), [WebLinkTests](../../test/WebLinkTests.cpp),
  [webfront-specs](../../webtest/js/webfront-specs.mjs).
- Voir [ADR-003](ADR-003-protocole-binaire-et-bornes.md) et [ADR-006](ADR-006-taches-structurees-et-arret.md).
