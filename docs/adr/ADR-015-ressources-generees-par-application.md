# ADR-015 : Générer les ressources embarquées dans le projet de l'application

## Versions

| Version | Date | État | Auteur | Évolution | Commit |
| --- | --- | --- | --- | --- | --- |
| 0.1 | 2026-10-20 | Proposée | Ambroise Leclerc | Proposition et première implémentation de wf embed et du parcours hors ligne. | À renseigner après intégration. |

## Statut et date

**Proposée le 2026-10-20.** Le parcours PI démontre une génération navigateur, sa compilation puis
l'exécution hors projet/cache avec WKWebView. Les autres moteurs nécessitent leur validation.

## Contexte

Les providers React/Babel fournis par WebFront imposent une sélection et une maintenance de
bibliothèques au projet. L'application doit pouvoir choisir ses versions et n'embarquer que les
ressources construites pour son interface, sans déplacer une chaîne Node chez son développeur.

## Décision proposée

`wf embed <entrée> --output <Provider.hpp>` construit les ressources avec le graphe installé,
ajoute la page HTML et produit un provider header-only `webfront::fs::<Provider>`. Il contient
JS/CSS/ressources compressées avec les paramètres fflate déterministes existants : niveau 9,
mem 8, mtime 0, puis mots 64 bits big-endian, padding exclu de `dataSize`, encodage gzip.
Les routes conservent leurs extensions et donc leurs types MIME, dont `application/wasm`.

Le provider conserve les notices des dépendances et leurs versions/intégrités. Sa génération ne
contient ni date courante, ni chemin absolu du poste, ni identifiant aléatoire. Le C++ reçoit les
chunks dans un staging, puis remplace le header une fois l'écriture achevée. Les chemins relatifs
et leurs parents existants sont vérifiés pour refuser traversal et symlinks sortant du projet.

La recompilation reste une action explicite du développeur. La distribution utilise
`NativeAppWithAssets<Provider>` sans projet local, callbacks de développement ni téléchargement.
Le bridge embarqué reste celui de WebFront, en priorité sur les providers applicatifs.
Conserver les providers historiques pour les consommateurs existants ; leur retrait n'est pas
une conséquence automatique de cette proposition.

## Alternatives et conséquences

- Distribuer le cache et des fichiers locaux maintiendrait une dépendance au poste de développement.
- Générer par une chaîne Node obligatoire contredirait l'autonomie du parcours visé.
- Le header et les temps de compilation croissent avec l'interface ; le coût appartient au projet
  qui choisit ces dépendances. Le binaire final n'embarque pas l'outillage de construction.
- Ce provider ne garantit pas que l'application finale n'effectue aucun accès réseau : la démo
  utilise uniquement des imports assemblés localement, mais le code applicatif reste responsable
  de ses éventuels appels réseau.

## Règles et vérification

Tester les octets décompressés, le padding, la stabilité à entrées identiques et les erreurs de
publication. Compiler le header réellement produit par le navigateur dans un second exécutable,
puis démarrer celui-ci depuis un répertoire vide, sans racine locale ni callbacks développeur.
Le test vérifie l'interface React et le résultat PI via le bridge.

## Références

- [Environnement développeur](../DEVELOPER_ENVIRONMENT.md), [ADR-009](ADR-009-modules-et-assets-reproductibles.md),
  [ADR-014](ADR-014-construction-javascript-dans-le-browser.md), [ADR-002](ADR-002-systemes-de-fichiers-composables.md).
