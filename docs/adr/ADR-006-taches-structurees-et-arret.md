# ADR-006 : Posséder, interrompre et joindre chaque tâche

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-10-13 | Acceptée | Ambroise Leclerc | Décision initiale : drainage des opérations asynchrones avant destruction du serveur. | [8f09ba5](https://github.com/ambroise-leclerc/WebFront/commit/8f09ba53bacc9468c5dfdaa3576c3d89d2d16ccc) |
| 1.1 | 2026-10-13 | Acceptée | Ambroise Leclerc | Fermeture des WebSockets et terminaison des opérations avant destruction de WebFront. | [b4fa321](https://github.com/ambroise-leclerc/WebFront/commit/b4fa321e6c10c296a0807d56b007f4cd9ec5635c) |
| 2.0 | 2026-10-18 | Acceptée | Ambroise Leclerc | Portée structurée commune, tâches applicatives possédées, TaskHandle et annulation locale des appels sender. | [0657923](https://github.com/ambroise-leclerc/WebFront/commit/0657923697ef697019f2a898754740971e79a79f) |

## État et chronologie

**Acceptée.** Décision initiale : **2026-10-13** (version 1.0).
Dernière révision : **2026-10-18** (version 2.0).
Le présent texte expose la décision en vigueur ; le tableau retrace ses états antérieurs.

## Contexte

L’arrêt du contexte ne garantit pas que les opérations asynchrones aient libéré leurs
références. Les correctifs de destruction précédèrent donc la portée structurée commune
aux opérations réseau et aux tâches applicatives.

## Décision

Le serveur possède un `counting_scope` (`networking::TaskScope`) couvrant acceptation, HTTP,
WebSocket et tâches applicatives. Une connexion possède sa source d’arrêt, liée à celle du
serveur : fermer une connexion n’arrête pas les autres.

`webfront::Task` et `app.spawn(task)`/`ui.spawn(task)` exécutent les coroutines sur le contexte
serveur. Un `TaskHandle` conservé permet `requestStop()` ; le jeter ne supprime pas la tâche.
Spawn et demande d’arrêt sont autorisés depuis tout thread tant que l’application vit.
Un spawn refusé pendant l’arrêt renvoie un handle déjà arrêté sans exécuter la coroutine.

`stop()` demande l’arrêt ; `shutdown()` arrête et joint. Après le retour des appelants externes
de `run()`/`runOne()`, le propriétaire draine avant destruction des dépendances. `drain()`
ferme le scope et pompe le contexte jusqu’à la fin du join. Un join sans progression possible
lève explicitement `std::logic_error`. Le destructeur conserve les propriétaires et attend,
sans propager cette erreur : une opération non coopérative peut le bloquer indéfiniment.

Un sender de fonction JS possède ses arguments encodés et envoie au démarrage. L’annulation
termine localement en `set_stopped()` ; le JavaScript déjà envoyé continue. Une réponse tardive
est écartée et son identifiant reste réservé jusqu’à réponse ou déconnexion. Une seule issue
prévaut entre résultat, arrêt et déconnexion.

## Options écartées et motifs

- Tâches détachées ou destruction dès `stop()` : risque d’accès à des propriétaires détruits.
- Scope possédé par chaque WebSocket : sa dernière tâche pourrait détruire le scope avant
  d’avoir relâché son association ; le scope est donc extérieur à la connexion.
- Commande d’annulation distante : non implémentée ; futures et Promises n’ont pas d’API
  d’annulation. Ne pas présenter l’arrêt local comme l’interruption d’un calcul navigateur.

## Conséquences

- **Bénéfice :** propriétaire et frontière de fin explicites ; arrêt individuel composable.
- **Coût accepté :** opérations coopératives requises, arrêt potentiellement bloquant.
- **Risque :** épuisement des identifiants 16 bits si les appels annulés ne répondent jamais ;
  signaler l’erreur plutôt que réutiliser un identifiant encore réservé.
- **Risque :** le sender termine sur le thread livrant résultat/arrêt/déconnexion ; lui-même ne
  promet pas de saut de scheduler. Les tâches lancées par WebFront reprennent sur son contexte.

## Prescriptions d’implémentation et de génération de code

- Que `shutdown()` soit appelé hors des callbacks serveur, après le retour des appelants
  externes de `run()` et `runOne()`.
- Dans une coroutine, que les données soient possédées ou passées par valeur ; que les lambdas
  coroutines temporaires ne retiennent aucune capture dont la fermeture disparaîtrait trop tôt.
- Les utilisateurs bas niveau de WebSocket/WebLink fournissent un `TaskScope&` extérieur et
  le drainent avant de libérer contexte et dépendances.
- Ne pas ajouter de threads détachés, délais de réponse implicites ou abandon de coroutine à
  la destruction. Synchroniser les états partagés et la destruction avec les appelants externes.
- Tester les courses arrêt/résultat, spawn/arrêt, double terminaison et réponse tardive.

## Sources et vérification

- [Contrat détaillé](../ASYNC_EXECUTION.md), [TaskScope](../../include/networking/Tasks.hpp).
- Tests : [StructuredShutdownTests](../../test/StructuredShutdownTests.cpp),
  [ApplicationTaskTests](../../test/ApplicationTaskTests.cpp), [JsFunctionSenderTests](../../test/JsFunctionSenderTests.cpp).
- Exemple compilé : [AwaitBrowser.cpp](../../src/AwaitBrowser.cpp).
