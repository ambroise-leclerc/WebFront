# ADR-014 : Construire les ressources web dans le moteur du navigateur

## Versions

| Version | Date | État | Auteur | Évolution | Commit |
| --- | --- | --- | --- | --- | --- |
| 0.1 | 2026-10-20 | Proposée | Ambroise Leclerc | Proposition et première implémentation du résolveur npm et du compilateur navigateur. | À renseigner après intégration. |

## Statut et date

**Proposée le 2026-10-20.** Validation initiale avec React et WKWebView ; couverture multiplateforme
et étendue de compatibilité des paquets à poursuivre avant acceptation générale.

## Contexte

Le gestionnaire d'outils de l'ADR-010 installe des outils déjà assemblés, sans dépendances. Le nouveau
parcours doit aussi installer des bibliothèques npm ordinaires et construire un TSX local sans Node.
Il ne s'agit pas de fournir toutes les API Node.js ou toutes les commandes npm dans un navigateur.

## Décision proposée

Séparer l'installation des bibliothèques applicatives du store des outils existants. Télécharger
métadonnées et archives avec le navigateur, vérifier SHA-512, refuser scripts d'installation et
extensions natives, résoudre les plages semver et garder les arêtes du graphe ainsi que plusieurs
versions d'un même paquet. Les peers requis doivent être satisfaits par des racines explicites ou
dépendances directes compatibles ; un conflit est une erreur. Les dépendances optionnelles ne sont
pas installées dans cette première version.

`webfront-packages.lock.json` enregistre racines, versions exactes, intégrités et graphe. Les sources
sont conservées dans un cache par nom/version/intégrité. L'écriture du lock constitue le point de
publication de l'installation ; un échec peut laisser du cache inutilisé, sans changer le lock.

Un Worker exécute esbuild/WASM pour les entrées JS/TS/JSX/TSX, CSS et ressources. Un filesystem
virtuel résout les imports du projet et des paquets, leurs exports navigateur et modules CommonJS.
Les lectures du projet transitent par les callbacks C++ bornés existants. Les imports réseau, API
Node absentes et exports privés échouent explicitement. Les bibliothèques ne sont pas exécutées
pour découvrir leurs exports ou lancer leurs scripts d'installation.

`wf preview` publie les résultats puis les affiche dans la même fenêtre. L'aperçu utilise le bridge
ordinaire, y compris les erreurs et les Promises. Ctrl+C termine un build navigateur ; aucun
processus C++ de compilation ou shell n'est lancé. La transformation TS n'effectue pas de typecheck.

## Alternatives et conséquences

- Un service distant de bundling introduirait une dépendance de construction externe et un autre
  contrat de confiance ; les sources sont ici construites localement.
- Des polyfills Node généraux ou l'émulation de scripts npm étendraient fortement le contrat ; ils
  restent hors périmètre. Les paquets doivent être utilisables dans un navigateur.
- Le compilateur WASM est téléchargé à la demande ; son poids n'affecte pas le binaire initial.
- Une nouvelle installation peut recalculer des plages transitives ; le diff du lock doit être
  revu. Preview/embed réutilisent exactement le graphe local déjà verrouillé. Une commande de
  restauration du cache depuis le lock sur une nouvelle machine reste à concevoir.

## Règles et vérification

Ne pas aplatir toutes les versions sous un nom unique. Préserver l'installation existante après
échec ou annulation. Borner téléchargements, décompression, nombre de paquets et temps de build.
Tester transitivité, cycles, versions multiples, peers, intégrité, exports conditionnels et imports
non disponibles. La preuve navigateur doit utiliser le vrai compilateur WASM et un registre local.

## Références

- [Environnement développeur](../DEVELOPER_ENVIRONMENT.md), [ADR-013](ADR-013-bootstrap-developpeur-a-la-demande.md),
  [ADR-003](ADR-003-protocole-binaire-et-bornes.md), [ADR-010](ADR-010-composants-et-outils-optionnels.md).
