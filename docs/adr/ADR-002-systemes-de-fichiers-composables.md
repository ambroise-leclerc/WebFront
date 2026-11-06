# ADR-002 : Un système de fichiers abstrait et composable, fondement de la portabilité

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2023-03-16 | Acceptée | Ambroise Leclerc | Décision initiale : système de fichiers injecté et ressources embarquées avec IndexFS. | [dd463b4](https://github.com/ambroise-leclerc/WebFront/commit/dd463b44407f3dca13f2f55d3f10d904e2653dc4) |
| 1.1 | 2023-03-22 | Acceptée | Ambroise Leclerc | Ressources encodées et prise en compte des capacités HTTP du client. | [3133e68](https://github.com/ambroise-leclerc/WebFront/commit/3133e68332a2fc658ab3431612fc08c792204ad0) |
| 1.2 | 2023-05-01 | Acceptée | Ambroise Leclerc | Composition ordonnée de fournisseurs avec Multi. | [8954c22](https://github.com/ambroise-leclerc/WebFront/commit/8954c2256354005f7e7d469b3ef98ae193c746cc) |
| 1.3 | 2023-05-08 | Acceptée | Ambroise Leclerc | Fournisseur natif réservé au développement : NativeDebugFS. | [56c647d](https://github.com/ambroise-leclerc/WebFront/commit/56c647d74c20a6efabe3460ed44ddbba263a12dd) |
| 1.4 | 2023-05-08 | Acceptée | Ambroise Leclerc | Unification des données embarquées et des flux std::ifstream dans File. | [318e890](https://github.com/ambroise-leclerc/WebFront/commit/318e890acde72393801b42141512f208741d055d) |
| 2.0 | 2023-05-10 | Acceptée | Ambroise Leclerc | Fournisseurs instanciés : abandon du contrat exclusivement statique. | [6af18a8](https://github.com/ambroise-leclerc/WebFront/commit/6af18a874d73761504055e575f15f96bc7be45eb) |
| 2.1 | 2026-07-22 | Acceptée | Ambroise Leclerc | Système de fichiers par défaut, bridge protégé et racine locale optionnelle. | [24fce3a](https://github.com/ambroise-leclerc/WebFront/commit/24fce3a993a07d73925660da7a74c195184293cc) |
| 2.2 | 2026-07-22 | Acceptée | Ambroise Leclerc | Page de repli embarquée prête pour le bridge. | [6e73219](https://github.com/ambroise-leclerc/WebFront/commit/6e73219a093c9bbe5c3f14bf0ac74e8e79e2803b) |
| 2.3 | 2026-10-14 | Acceptée | Ambroise Leclerc | Composition facultative des ressources des composants avec DefaultFSWith. | [40f5621](https://github.com/ambroise-leclerc/WebFront/commit/40f5621928fdd188f230bf08ff062f9557619046) |
| 2.4 | 2026-10-14 | Acceptée | Ambroise Leclerc | Fournisseur PackageFS et stockage transactionnel des outils installés. | [4bcfc28](https://github.com/ambroise-leclerc/WebFront/commit/4bcfc28c7b78672603a2114a72d9decf25d36387) |
| 2.5 | 2026-11-03 | Acceptée | Ambroise Leclerc | `fs::File::knownSize()` : taille connue avant lecture (données embarquées, fichier natif sur flux positionnable), pour annoncer `Content-Length` et envoyer le fichier en flux (#454). Les routes dynamiques passent après le `WebFront.js` protégé et ne peuvent pas le masquer. | Branche `454-dynamic-routes` |
| 2.6 | 2026-11-04 | Acceptée | Ambroise Leclerc | `fs::File::seek`, `entityTag()` et `setEntityTag()` : positionnement dans un fichier de taille connue et validateur fort fourni par le fournisseur (jamais déduit d'un chemin, d'une taille ou d'une date), pour les requêtes `Range` et `If-Range` (#455). Le serveur traite les plages ; `fs::Provider` ne voit toujours pas la requête. | Branche `455-range-requests` |
| 3.0 | 2026-11-05 | Acceptée | Ambroise Leclerc | Stockage modifiable sous des racines déclarées (#460, ADR-020) : concept `fs::Storage` (listage paginé stable, métadonnées, lecture, écriture par fichier temporaire validée par renommage atomique, création de dossier, suppression, renommage sans remplacement), erreurs typées `fs::StorageError` (`storage.*`), `fs::NativeStorage` confiné pendant la résolution par descripteurs (POSIX) ou handles (Windows), contrôle des propriétaires des répertoires et des ancêtres, `rootMoved` et `confinementViolated`, `fs::MemoryStorage` simulé ; `fs::ByteSource`, point d'extension de `fs::File` pour les handles natifs ; `SafePath` refuse les noms réservés de Windows. Les fournisseurs restent en lecture seule. | Branche `460-writable-storage` |

## État et chronologie

**Acceptée.** Décision initiale : **2023-03-16** (version 1.0).
Dernière révision : **2026-11-05** (version 3.0).
Le présent texte expose la décision en vigueur ; le tableau retrace ses états antérieurs.

## Contexte

Le serveur HTTP doit servir l’interface sans hériter de l’arborescence du poste de
développement. Cette frontière fondamentale permet que les mêmes pages et le même pont
proviennent du binaire, d’une image système, d’un volume en lecture seule ou d’un stockage
propre à la plateforme.

Cette indépendance rend envisageables les déclinaisons embarquées, les images Linux construites
avec Yocto et les systèmes mobiles. La politique de stockage appartient
à l’application et à son intégration système ; elle ne doit pas se propager au serveur HTTP,
au protocole du bridge ou aux composants UI. L’usage actuel des providers sert déjà cette
orientation ; les portages et packagings correspondants restent à réaliser et à vérifier.

Les fichiers modifiables ne sont qu’un usage de cette abstraction. Il faut en outre que le
script du pont demeure conforme au codec C++, même si une racine locale contient un autre
`WebFront.js`.

Jusqu’à la version 2.6, cette décision ne définissait que la lecture (`open → fs::File`). Les services du
système (ADR-020) doivent aussi lister, écrire, créer, renommer et supprimer des fichiers de l’hôte, sous des
racines que l’application déclare, sans qu’un script de la page ni un processus local sans droit sur ces racines
puisse les faire agir à l’extérieur. Une vérification du chemin suivie d’une ouverture par nom ne suffit pas :
entre les deux, un processus local peut remplacer un composant par un lien vers l’extérieur.

## Décision

Dissocier les ressources web du système de fichiers physique. Le serveur
consomme un filesystem injecté ; il ne choisit ni répertoire d’installation, ni montage,
ni emplacement de ressources mobiles. Une URL désigne d’abord une ressource logique,
éventuellement adossée à un fichier natif.

Un provider implémente `open(std::filesystem::path) -> std::optional<fs::File>` et satisfait
le concept `fs::Provider`, qui exige aussi sa construction depuis un chemin. `std::nullopt`
signifie que ce provider ne fournit pas la ressource ; `fs::Multi<...>` essaie les providers
dans l’ordre et choisit le premier résultat. `fs::File` unifie lecture par blocs, fin de
fichier et métadonnées d’encodage, indépendamment du choix du provider.

La réalisation actuelle de `fs::File` accepte les données statiques embarquées et les
`std::ifstream`. Cette réalisation ne constitue pas encore une interface de flux universelle :
un provider utilisant
un handle natif mobile ou un stockage spécialisé devra adapter ses octets à ces formes, ou
faire évoluer explicitement `File` avec ses tests. Le `span` des données statiques emprunte
leur stockage, qui doit survivre à la lecture.

`fs::File` lit aussi les octets d’un `fs::ByteSource`, que fournit un stockage ou un provider lisant par son propre
handle : un descripteur ouvert relativement à une racine (`fs::NativeStorage`), plus tard une URI `content://`
d’Android ou une URL « security-scoped » de macOS, que WebFront n’a pas à connaître. Ces handles natifs ne sont
pas implémentés ; `ByteSource` est leur point d’extension.

Le filesystem par défaut consulte, dans cet ordre : le `WebFront.js` embarqué protégé,
la racine de développement optionnelle, puis la page de repli embarquée d’`IndexFS`.
`fs::DefaultFSWith<Extra...>` place les providers supplémentaires entre le bridge protégé
et la racine locale. Sans `documentRoot`, le provider local ne touche pas au disque.

Les espaces réservés `webfront/components/<name>/` et `webfront/tools/<pkg>/` séparaient les
ressources réutilisables des pages applicatives ; `PackageFS` et les composants qui les servaient
sont retirés (ADR-010 v1.7), l'environnement développeur sert les siennes sous `webfront/developer/`. Les fichiers natifs de développement et `NativeDebugFS`
ne constituent pas un sandbox de production.

### Stockage modifiable sous des racines déclarées

Un fournisseur sert des ressources en lecture ; un **stockage** (`fs::Storage`, `Storage.hpp`) est modifiable et
confiné à une racine. Ce sont deux contrats distincts : le serveur HTTP ne consomme que des fournisseurs, et un
stockage n’est servi que par une route à jeton de l’ADR-020.

- **Contrat.** Chemins relatifs séparés par `/`, vérifiés par `SafePath` (le chemin vide désigne la racine) ;
  `list(dir, after, limit)` rend une page triée par nom, au plus `maxStoragePageSize` (1000) entrées, et un curseur :
  une entrée présente pendant tout le listage y figure exactement une fois, quoi qu’on ajoute ou retire entre deux
  pages ; `status` (un lien n’est pas suivi), `openRead`, `beginWrite(path, createNew | replace)` qui écrit dans un
  fichier temporaire rendu visible par un renommage atomique à `commit()` et supprimé si l’écrivain est détruit avant,
  `createDirectory`, `remove` (fichier, lien ou dossier vide), `rename` (jamais de remplacement). Les noms commençant
  par `.wf-tmp-` (`fs::temporaryPrefix`), quelle que soit la casse de ses lettres (un volume insensible à la casse
  désigne le même fichier par `.WF-TMP-`), sont réservés aux temporaires : refusés (`invalidPath`) et omis des listages,
  de sorte qu’un listage ne masque jamais un fichier validé. Le concept ne fait
  aucune hypothèse POSIX : un stockage sans `std::filesystem` (FatFs, LittleFS) peut le satisfaire.
- **Erreurs typées.** `fs::StorageError` est une `BridgeError` (ADR-004) de code `storage.<nom>` : `notFound`,
  `alreadyExists`, `notDirectory`, `isDirectory`, `notEmpty`, `accessDenied`, `invalidPath`, `linkRefused`,
  `unsafeDirectory`, `rootMoved`, `confinementViolated`, `busy`, `ioError`. Son message nomme le chemin relatif,
  jamais l’emplacement de la racine sur l’hôte. Les droits rapportés par `status` et le listage sont indicatifs (bits
  de mode POSIX, attribut lecture seule de Windows ; les ACL n’y sont pas examinées).
- **Syntaxe.** `SafePath` refuse en outre, sur toutes les plateformes, les noms de périphériques réservés de Windows
  (`CON`, `PRN`, `AUX`, `NUL`, `COM1` à `COM9`, `LPT1` à `LPT9`, `CONIN$`, `CONOUT$`, quelle que soit l’extension),
  et les noms terminés par un point ou une espace, que Windows modifie. Un nom qu’il
  refuse n’est pas listé, puisqu’aucune opération ne pourrait le désigner. La syntaxe ne confine pas : elle suffit
  seulement aux stockages sans liens.
- **`fs::NativeStorage`.** La racine est la forme canonique de l’emplacement déclaré (ses liens sont résolus une fois,
  à la déclaration). Chaque opération repart de la racine ouverte et ouvre chaque composant relativement au précédent,
  sans suivre de lien (POSIX : `openat(O_NOFOLLOW | O_DIRECTORY)`, `O_PATH` sous Linux, `O_SEARCH` sous macOS ;
  Windows : `NtCreateFile` relatif au handle parent, `FILE_OPEN_REPARSE_POINT`, points de réanalyse refusés) ; le
  dernier composant est traité par `openat(O_CREAT | O_EXCL | O_NOFOLLOW)`, `fstatat(AT_SYMLINK_NOFOLLOW)`,
  `mkdirat`, `unlinkat`, `renameat`, renommage sans remplacement par `renameat2(RENAME_NOREPLACE)` (Linux) ou
  `renameatx_np(RENAME_EXCL)` (macOS) ; sur un système de fichiers Linux sans `renameat2`, un fichier est lié puis
  délié (`link` ne remplace jamais) et un dossier est refusé, faute de primitive atomique. Sous Windows, le renommage
  passe par `NtSetInformationFile` relatif au handle du dossier (`SetFileInformationByHandle` convertit le nom en
  chemin absolu, que ce renommage refuse), la suppression par disposition. Aucun descripteur ne survit à l’opération. `openat2(RESOLVE_BENEATH)` n’est
  pas utilisé : chaque répertoire traversé doit de toute façon être ouvert pour contrôler son propriétaire.
  Les liens sont refusés par défaut (`linkRefused`) ; `LinkPolicy::followInside` suit sous POSIX un lien relatif dont
  la résolution, faite par la même descente, reste dans la racine (au plus 32 liens) ; un lien absolu ou qui sort est
  refusé. Sous Windows, tout point de réanalyse est refusé.
- **Emplacement déclaré.** Chaque opération vérifie d’abord que l’emplacement déclaré, ouvert composant par composant
  sans suivre de lien, désigne encore l’identité de la racine (`st_dev`/`st_ino`, `FILE_ID_INFO`) ; sinon `rootMoved`,
  pour cette opération et les suivantes, jusqu’à une nouvelle déclaration. Seuls un composant absent, qui n’est pas un
  répertoire ou qui est un lien, et une identité différente, valent déplacement ; une autre erreur (descripteurs
  épuisés, mémoire, droits) est signalée comme telle et ne verrouille pas la racine.
- **Propriétaires (POSIX).** Chaque répertoire traversé et la racine appartiennent à l’utilisateur effectif ou au
  superutilisateur et ne sont modifiables ni par le groupe ni par les autres ; chaque ancêtre appartient à l’un d’eux
  et, s’il est modifiable par le groupe ou les autres, porte le bit collant et l’élément suivant appartient aussi à
  l’un d’eux (cas de `/tmp`). Sous macOS, une ACL étendue qui accorde à un autre que ces deux utilisateurs le droit
  d’ajouter, de retirer ou de renommer des entrées, de supprimer le répertoire ou de changer son ACL ou son
  propriétaire rend aussi le répertoire ou l’ancêtre inadmissible (une ACL illisible compte comme telle) ; sous Linux,
  une entrée nommée d’ACL POSIX qui accorde l’écriture positionne le bit d’écriture du groupe (masque) et tombe sous la
  règle des bits. Contrôle à la déclaration et à chaque opération, avant tout effet ; sinon `unsafeDirectory`. Les dossiers créés le sont en mode 0755, les fichiers en 0644 (masqués par l’umask), pour rester
  utilisables. `RootOptions::shared` lève ces contrôles : confinement affaibli, pour un dossier partagé ou un support
  monté sans permissions.
- **Windows.** La racine est tenue sans `FILE_SHARE_DELETE` pendant toute la déclaration, chaque répertoire traversé
  pendant toute l’opération (écriture du temporaire et renommage compris) : leur renommage ou leur suppression
  échoue. Un fichier remplacé reçoit sous Windows l’ACL héritée du dossier, pas celle de l’ancien fichier (limite
  connue) ; sous POSIX, il prend, à la validation et sous le verrou du stockage, le groupe (refus si l’utilisateur
  effectif n’en est pas membre), les bits de permission (sans setuid, setgid ni bit collant) et l’ACL (ACL étendue de macOS, ACL d’accès POSIX de Linux) du fichier qu’il remplace à ce
  moment, et aucune ACL si ce fichier n’en a pas, même si son dossier en transmet une ; le temporaire reste en 0600,
  sans l’ACL héritée de son dossier (retirée avant toute écriture), jusque-là. Si cette ACL ne peut être lue ou recopiée (sous macOS, un fichier que l’utilisateur effectif ne peut pas
  lire), le remplacement est refusé (`accessDenied`). Son propriétaire devient l’utilisateur
  effectif. Un lien suivi dans la racine ne peut pas désigner un temporaire (`linkRefused`). NTFS refuse aussi le renommage d’un ancêtre sous lequel un handle est ouvert ; le test le vérifie. Coût
  accepté : la racine ne peut pas être renommée tant qu’elle est déclarée.
- **Détection.** Avant de valider une écriture et après toute opération, le stockage vérifie que le répertoire où il
  a agi est encore sous la racine (remontée par `..` jusqu’à l’identité de la racine sous POSIX, chemin final du
  handle sous Windows) et que la racine est à son emplacement ; sinon `confinementViolated`, la validation n’a pas lieu
  et le temporaire est supprimé. Les renommages, validations, suppressions et créations d’un stockage sont sérialisés.
- **Garantie.** Windows : empêchement pendant toute l’opération. POSIX : empêchement face à tout autre utilisateur que
  l’utilisateur effectif et le superutilisateur ; face à eux, détection au mieux : un déplacement entre la dernière
  vérification et le renommage, ou pendant l’écriture du temporaire après la vérification, peut laisser un fichier à
  l’extérieur.
- **`fs::MemoryStorage`** est le stockage simulé des tests (arbre en mémoire, droits simulés par `setAccess`), et le
  modèle d’un stockage sans liens. `fs::StorageHooks` est réservé aux tests : points d’injection entre deux étapes
  de résolution, avant l’effet, après la création du temporaire et avant la validation, et substitution de
  l’utilisateur effectif ou du propriétaire d’un répertoire.

## Orientation embarquée, Yocto et mobile

| Déclinaison visée | Conséquence de la séparation | Travail d’intégration restant |
| --- | --- | --- |
| Embarqué | Une UI embarquée dans le binaire peut être servie sans répertoire web inscriptible. | Choisir les ressources, le budget mémoire et un backend réseau/frontend adaptés à la cible. |
| Linux/Yocto | Le provider peut lire un contenu installé dans l’image ou des ressources intégrées ; les URLs restent identiques. | Définir recette, cross-compilation, dépendances, emplacement et droits des ressources ; les tester sur l’image cible. |
| OS mobiles | L’accès aux ressources de bundle/package et aux répertoires autorisés reste confiné dans le provider. | Fournir les adaptations de stockage et de frontend, gérer le cycle de vie de l’OS et qualifier la chaîne de compilation. |

Ces déclinaisons inspirent l’architecture ; aucune n’est encore qualifiée.
Le seul filesystem ne résout pas les dépendances réseau, navigateur, système ou toolchain.
La composition reste explicite : pas de recherche automatique dans les répertoires personnels,
pas d’écriture implicite pour servir une page. Les outils de développement qui écrivent dans
un store restent optionnels et demandent un emplacement adapté à la cible.

## Options écartées et motifs

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

## Prescriptions d’implémentation et de génération de code

- Pour toute nouvelle cible, que soient définis d’abord son fournisseur de ressources et leur
  composition ; que les `#ifdef` de stockage ne gagnent ni HTTP, ni le pont, ni les composants.
- Que `std::filesystem::path` ne soit jamais tenu pour une autorisation d’accès au disque :
  seul le fournisseur décide comment résoudre et ouvrir la ressource.
- Documenter stockage possédé ou emprunté, volume lu, coût des lectures et droits nécessaires.
  Tester le provider indépendamment du navigateur, puis le trajet HTTP sur la cible.
- Étendre par un provider ciblé, sans contourner `Multi` ni rendre le bridge remplaçable.
- Tester priorité, absence de fichier, absence de racine et lecture partielle/EOF.
- Respecter le traitement URI de la couche HTTP : ne pas utiliser query ou fragment comme nom
  de fichier. Ne pas introduire une seconde convention de résolution dans un composant.
- Conserver métadonnées MIME/encodage et taille servie cohérentes ; voir ADR-009 pour gzip.
- Ne jamais confiner une écriture ou une lecture de l’hôte par une vérification de chemin suivie d’une ouverture par
  nom : passer par un `fs::Storage`, dont chaque opération résout son chemin relativement à la racine ouverte.
- Ne pas présenter `RootOptions::shared` ni un stockage sans liens comme un confinement complet ; documenter la
  garantie POSIX (aucun empêchement face au même utilisateur ou au superutilisateur).
- Un nouveau stockage satisfait `fs::Storage`, lève `fs::StorageError` et se teste comme `MemoryStorage` ; un
  stockage natif ajoute les tests de liens, de remplacement et de déplacement concurrents par `StorageHooks`.

## Sources et vérification

- [DefaultFS](../../include/webfront/system/DefaultFS.hpp), [FileSystem](../../include/webfront/system/FileSystem.hpp),
  [NativeFS](../../include/webfront/system/NativeFS.hpp), [Storage](../../include/webfront/system/Storage.hpp),
  [NativeStorage](../../include/webfront/system/NativeStorage.hpp), [MemoryStorage](../../include/webfront/system/MemoryStorage.hpp),
  [SafePath](../../include/webfront/system/SafePath.hpp).
- Tests : [FileSystemTests](../../test/unit/FileSystemTests.cpp), [DefaultFSTests](../../test/unit/DefaultFSTests.cpp),
  [ModuleHTTPServerTests](../../test/unit/ModuleHTTPServerTests.cpp), [StorageTests](../../test/unit/StorageTests.cpp),
  [NativeStorageTests](../../test/unit/NativeStorageTests.cpp), [SafePathTests](../../test/unit/SafePathTests.cpp).
- Décision qui motive le stockage modifiable : [ADR-020](ADR-020-services-systeme.md), règles 2 à 4 et prescriptions.
- Voir [ADR-009](ADR-009-modules-et-assets-reproductibles.md) et [ADR-010](ADR-010-composants-et-outils-optionnels.md).
