# ADR-002 : Un système de fichiers abstrait et composable comme fondation de la portabilité

## Versions

| Version | Date | État | Auteur | Évolution | Commit |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2023-03-16 | Acceptée | Ambroise Leclerc | Création initiale : filesystem injecté et ressources embarquées avec IndexFS. | [dd463b4](https://github.com/ambroise-leclerc/WebFront/commit/dd463b44407f3dca13f2f55d3f10d904e2653dc4) |
| 1.1 | 2023-03-22 | Acceptée | Ambroise Leclerc | Ressources encodées et prise en compte des capacités HTTP du client. | [3133e68](https://github.com/ambroise-leclerc/WebFront/commit/3133e68332a2fc658ab3431612fc08c792204ad0) |
| 1.2 | 2023-05-01 | Acceptée | Ambroise Leclerc | Composition ordonnée de providers avec Multi. | [8954c22](https://github.com/ambroise-leclerc/WebFront/commit/8954c2256354005f7e7d469b3ef98ae193c746cc) |
| 1.3 | 2023-05-08 | Acceptée | Ambroise Leclerc | Provider natif réservé au développement : NativeDebugFS. | [56c647d](https://github.com/ambroise-leclerc/WebFront/commit/56c647d74c20a6efabe3460ed44ddbba263a12dd) |
| 1.4 | 2023-05-08 | Acceptée | Ambroise Leclerc | Unification des données embarquées et des flux std::ifstream dans File. | [318e890](https://github.com/ambroise-leclerc/WebFront/commit/318e890acde72393801b42141512f208741d055d) |
| 2.0 | 2023-05-10 | Acceptée | Ambroise Leclerc | Providers instanciés : abandon du contrat exclusivement statique. | [6af18a8](https://github.com/ambroise-leclerc/WebFront/commit/6af18a874d73761504055e575f15f96bc7be45eb) |
| 2.1 | 2026-07-22 | Acceptée | Ambroise Leclerc | Filesystem par défaut, bridge protégé et racine locale optionnelle. | [24fce3a](https://github.com/ambroise-leclerc/WebFront/commit/24fce3a993a07d73925660da7a74c195184293cc) |
| 2.2 | 2026-07-22 | Acceptée | Ambroise Leclerc | Page de repli embarquée prête pour le bridge. | [6e73219](https://github.com/ambroise-leclerc/WebFront/commit/6e73219a093c9bbe5c3f14bf0ac74e8e79e2803b) |
| 2.3 | 2026-10-14 | Acceptée | Ambroise Leclerc | Composition optionnelle des assets de composants avec DefaultFSWith. | [40f5621](https://github.com/ambroise-leclerc/WebFront/commit/40f5621928fdd188f230bf08ff062f9557619046) |
| 2.4 | 2026-10-14 | Acceptée | Ambroise Leclerc | Provider PackageFS et stockage transactionnel des outils installés. | [4bcfc28](https://github.com/ambroise-leclerc/WebFront/commit/4bcfc28c7b78672603a2114a72d9decf25d36387) |

## Statut et date

**Acceptée.** Création initiale : **2023-03-16** (version 1.0).
Dernière évolution : **2026-10-14** (version 2.4).
Le corps de l’ADR décrit la décision dans sa dernière version ; les versions antérieures
et leurs changements sont identifiés dans le tableau ci-dessus.

## Contexte

Le serveur HTTP doit servir une interface sans imposer l’organisation du disque d’un poste
de développement. C’est une frontière architecturale fondatrice : les mêmes pages et le même
bridge doivent pouvoir être fournis par des ressources liées au programme, une image système,
un volume en lecture seule ou un stockage propre à la plateforme.

Cette indépendance doit permettre de décliner WebFront sur de l’embarqué, une distribution
Linux construite avec Yocto et des OS mobiles. La politique de stockage appartient à
l’application et à son intégration système ; elle ne doit pas se propager au serveur HTTP,
au protocole du bridge ou aux composants UI. L’usage actuel des providers sert déjà cette
orientation ; les portages et packagings correspondants restent à réaliser et à vérifier.

Le développement sur fichiers éditables est un cas de cette abstraction, pas sa raison
d’être unique. Le script du bridge doit en outre rester cohérent avec le codec C++, même
si une racine locale contient un autre `WebFront.js`.

## Décision

Séparer l’espace de ressources web du système de fichiers physique de l’OS. Le serveur
consomme un filesystem injecté ; il ne choisit ni répertoire d’installation, ni montage,
ni emplacement de ressources mobiles. Une URL nomme une ressource logique avant de désigner,
éventuellement, un fichier natif.

Un provider implémente `open(std::filesystem::path) -> std::optional<fs::File>` et satisfait
le concept `fs::Provider`, qui exige aussi sa construction depuis un chemin. `std::nullopt`
signifie que ce provider ne fournit pas la ressource ; `fs::Multi<...>` essaie les providers
dans l’ordre et choisit le premier résultat. `fs::File` unifie lecture par blocs, fin de
fichier et métadonnées d’encodage, indépendamment du choix du provider.

La réalisation actuelle de `fs::File` accepte les données statiques embarquées et les
`std::ifstream`. Ce n’est pas encore une interface de flux arbitraire : un provider utilisant
un handle natif mobile ou un stockage spécialisé devra adapter ses octets à ces formes, ou
faire évoluer explicitement `File` avec ses tests. Le `span` des données statiques emprunte
leur stockage, qui doit survivre à la lecture.

Le filesystem par défaut consulte, dans cet ordre : le `WebFront.js` embarqué protégé,
la racine de développement optionnelle, puis la page de repli embarquée d’`IndexFS`.
`fs::DefaultFSWith<Extra...>` place les providers supplémentaires entre le bridge protégé
et la racine locale. Sans `documentRoot`, le provider local ne touche pas au disque.

Les espaces réservés `webfront/components/<name>/` et `webfront/tools/<pkg>/` séparent les
ressources réutilisables des pages applicatives. `PackageFS` ne sert que les installations
référencées par le store (ADR-010). Les fichiers natifs de développement et `NativeDebugFS`
ne constituent pas un sandbox de production.

## Orientation embarquée, Yocto et mobile

| Déclinaison visée | Conséquence de la séparation | Travail d’intégration restant |
| --- | --- | --- |
| Embarqué | Une UI embarquée dans le binaire peut être servie sans répertoire web inscriptible. | Choisir les ressources, le budget mémoire et un backend réseau/frontend adaptés à la cible. |
| Linux/Yocto | Le provider peut lire un contenu installé dans l’image ou des ressources intégrées ; les URLs restent identiques. | Définir recette, cross-compilation, dépendances, emplacement et droits des ressources ; les tester sur l’image cible. |
| OS mobiles | L’accès aux ressources de bundle/package et aux répertoires autorisés reste confiné dans le provider. | Fournir les adaptations de stockage et de frontend, gérer le cycle de vie de l’OS et qualifier la chaîne de compilation. |

Ces déclinaisons motivent l’architecture ; elles ne sont pas des plateformes déjà qualifiées.
Le seul filesystem ne résout pas les dépendances réseau, navigateur, système ou toolchain.
La composition reste explicite : pas de recherche automatique dans les répertoires personnels,
pas d’écriture implicite pour servir une page. Les outils de développement qui écrivent dans
un store restent optionnels et demandent un emplacement adapté à la cible.

## Alternatives et arbitrage

- Coupler HTTP aux API de fichiers d’un OS ou aux chemins de l’image cible : empêcherait de
  réutiliser le serveur et les pages sur d’autres stockages ; confiner ce choix au provider.
- Dépendre d’un répertoire web externe obligatoire : exclurait le déploiement autonome depuis
  des données embarquées et compliquerait les images immuables.
- Utiliser systématiquement le répertoire courant : écarterait le démarrage sans accès local
  et rendrait le contenu servi dépendant du lancement.
- Donner priorité absolue à la racine locale : permettrait de remplacer le bridge par une
  version incompatible et de masquer des composants.
- Imposer une seule archive ou un framework de ressources : inutile pour le contrat de provider
  et les tests existants ; les nouveaux stockages peuvent implémenter le même concept.

## Conséquences

- **Bénéfice :** même serveur et mêmes URLs sur des stockages différents ; base commune aux
  déclinaisons desktop, embarquées, Yocto et mobiles. Les assets peuvent être liés au programme.
- **Coût accepté :** le provider doit traduire les conventions d’URL, gérer les droits et garantir
  la durée de vie des octets ; les contraintes mémoire et d’I/O restent propres à la cible.
- **Coût accepté :** l’ordre des providers affecte le comportement ; collisions à examiner.
- **Risque :** servir des fichiers non prévus avec une racine de développement ; ne pas en
  déduire une isolation de sécurité et conserver le bind loopback par défaut.

## Règles d’implémentation et de génération de code

- Pour une nouvelle cible, commencer par son provider et sa composition de ressources ; ne pas
  disperser des `#ifdef` de chemins/stockage dans HTTP, le bridge ou les composants.
- Ne pas assimiler `std::filesystem::path` à une autorisation d’accès disque : c’est le provider
  qui décide comment résoudre et ouvrir cette ressource.
- Documenter stockage possédé ou emprunté, volume lu, coût des lectures et droits nécessaires.
  Tester le provider indépendamment du navigateur, puis le trajet HTTP sur la cible.
- Étendre par un provider ciblé, sans contourner `Multi` ni rendre le bridge remplaçable.
- Tester priorité, absence de fichier, absence de racine et lecture partielle/EOF.
- Respecter le traitement URI de la couche HTTP : ne pas utiliser query ou fragment comme nom
  de fichier. Ne pas introduire une seconde convention de résolution dans un composant.
- Conserver métadonnées MIME/encodage et taille servie cohérentes ; voir ADR-009 pour gzip.

## Références et vérification

- [DefaultFS](../../include/system/DefaultFS.hpp), [FileSystem](../../include/system/FileSystem.hpp),
  [NativeFS](../../include/system/NativeFS.hpp).
- Tests : [FileSystemTests](../../test/FileSystemTests.cpp), [DefaultFSTests](../../test/DefaultFSTests.cpp),
  [ModuleHTTPServerTests](../../test/ModuleHTTPServerTests.cpp).
- Voir [ADR-009](ADR-009-modules-et-assets-reproductibles.md) et [ADR-010](ADR-010-composants-et-outils-optionnels.md).
