# ADR-026 : Configuration MCP stable en développement

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-11-22 | Proposée | Ambroise Leclerc | Configuration persistante par projet (#637). | — |
| 1.1 | 2026-11-22 | Acceptée, puis suspendue | Ambroise Leclerc | L'usurpation de l'écouteur révèle une exposition du jeton persistant. | — |
| 1.2 | 2026-11-22 | Acceptée (livraison en cours) | Ambroise Leclerc | Mode explicite simplifié, compromis de sécurité acceptés ; issue #637 amendée. | #637 |

## Décision

Le mode de l'ADR-025 reste le défaut : port choisi par l'OS, jeton aléatoire par session et configuration privée supprimée à la fermeture. L'application peut fixer le port et fournir un jeton Bearer durable, ou désactiver explicitement l'authentification. Le développeur gère le secret et sa rotation ; WebFront ne le persiste pas hors de la configuration privée de session. Le client copie une fois URL et en-tête dans sa configuration utilisateur, hors projet. Il doit lancer l'application avant de se connecter. Un port occupé provoque une erreur explicite ; une autre instance peut utiliser le mode par défaut. La découverte ne contient pas de jeton.

Ces options concernent uniquement le développement : l'automatisation reste exclue des builds de distribution. L'écoute reste sur loopback ; Host, Origin et Sec-Fetch-* restent vérifiés. Un jeton configuré est comparé en temps constant. Les valeurs malformées et les combinaisons ambiguës sont refusées.

## Conséquences

Un autre utilisateur local peut occuper le port pendant l'arrêt de l'application et récupérer un Bearer durable envoyé par le client : ce jeton restera utilisable jusqu'à rotation. Sans authentification, tout processus local pouvant joindre le port peut exercer les grants. Un serveur usurpé peut également renvoyer des réponses mensongères au client. Ces limites sont acceptées uniquement sur choix explicite. Host et Origin protègent contre les requêtes de pages web, pas contre ces processus. Un challenge non standard exigerait un client compatible ; le Bearer fonctionne avec les clients MCP ordinaires.

## Validation

Relancer l'application deux fois avec la même configuration cliente et appeler initialize puis webfront.session, avec le client HTTP du dépôt et le SDK 2.3.0 s'il est disponible. Vérifier les refus, les instances concurrentes, le conflit de port, l'absence de secret dans la découverte et les protections réseau dans les modes authentifié et sans authentification. Maintenir les guides et AgentGuidance.
