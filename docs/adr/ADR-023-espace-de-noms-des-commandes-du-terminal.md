# ADR-023 : Espace de noms des commandes du terminal développeur

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-11-14 | Acceptée | Ambroise Leclerc | Décision initiale (#575), avant la 1.0 : le préfixe `wf` est conservé ; le terminal reçoit des commandes de fichiers en lecture seule (`ls`, `cat`, `tree`, `mounts`) et réserve le vocabulaire de fichiers qu'il ne fournit pas ; `wf list --tree`, `wf why` et `wf info` montrent le graphe des dépendances. | À renseigner après intégration. |

## État et chronologie

**Acceptée** le **2026-11-14** par le propriétaire du projet, comme dernière décision d'architecture avant la 1.0. Elle
est mise en œuvre par la même modification : les noms réservés de `webfront::Commands`, le callback
`webfront.developer.entries` et l'environnement qui porte les commandes.

## Contexte

Le terminal de l'environnement développeur ([ADR-016](ADR-016-bootstrap-comme-socle.md)) route une ligne vers trois
familles de commandes :

- `wf <verbe>` : les commandes du projet (`init`, `create`, `install`, `restore`, `run`, `exec`, `check`, `test`, `build`,
  `dev`, `preview`, `embed`, `list`, `help`) ;
- les commandes du terminal lui-même (`help`, `clear`, `history`, `exit`) ;
- les commandes C++ que l'application ajoute avec `webfront::Commands`, sous le nom qu'elle choisit.

Les noms que le terminal traite lui-même sont refusés à l'application (`webfront::commands::reservedNames`, et la même
liste dans `commands.mjs`, comparées par le test `CommandNamesAgree`). Cette liste est un contrat public : un nom qui y entre
après la 1.0 casse l'application qui l'utilisait.

Deux questions se posaient. Taper `wf` devant chaque commande paraît répétitif : le préfixe est-il nécessaire ? Et le
terminal ne montrait rien du projet sur lequel il agit : ni ses fichiers, ni ce que l'application sert au navigateur, ni
les relations entre les paquets installés, dont `wf list` ne donnait que la liste.

## Décision

### 1. Le préfixe `wf` est conservé

Les noms nus appartiennent au terminal et à l'application ; `wf` est l'espace de noms du projet : son manifeste, son
lock, ses paquets et sa construction.

- **Les verbes de wf sont des mots courants.** Sans préfixe, `test`, `run`, `build`, `check`, `list`, `install`,
  `create` ou `help` seraient interdits aux commandes de l'application, qui sont justement celles qu'une application
  nomme ainsi. Avec le préfixe, la réserve ne contient que `wf` et le vocabulaire du terminal.
- **wf doit pouvoir grandir après la 1.0.** Un nouveau verbe (`wf why`, `wf info` dans cette décision même) n'ajoute
  aucun nom réservé. Sans préfixe, chaque verbe ajouté serait une rupture pour une application qui le porte.
- **`wf` n'est pas qu'une convention de saisie.** Le premier mot `wf` distingue, dans les scripts du manifeste, une
  commande d'un module (`"build": "wf embed LifeUI"`) ; c'est le nom du pilote non interactif (`webfront-wf`) et celui
  que citent la documentation, le guide des agents et les messages d'erreur (« run wf install »). Une ligne citée se
  copie telle quelle dans le terminal, dans un script ou dans une documentation.
- **Un gestionnaire de paquets se nomme.** `install`, `list`, `why`, `info` n'ont de sens que rapportés à un outil,
  comme pour les gestionnaires de paquets usuels. `wf list` (les paquets) et `ls` (les fichiers) ne se confondent pas.

Une commande nue inconnue qui est un verbe de wf n'est pas exécutée : l'erreur donne la ligne à taper
(`Unknown command: install. Did you mean: wf install react?`). Elle n'est pas résolue implicitement, car le sens d'une
ligne dépendrait alors des commandes de l'application.

### 2. Des commandes de fichiers en lecture seule

Le terminal fournit, sous des noms nus et dans le vocabulaire habituel :

| Commande | Rôle |
| --- | --- |
| `ls [-l] [-a] [chemin]` | les entrées d'un répertoire du projet ; `-l` ajoute le type, la taille, la date et la *nature* de l'entrée pour wf (manifeste, lock, entrée, spec, script, provider généré, cache et publications sous `.webfront/`) |
| `cat <fichier>...` | le texte UTF-8 de fichiers du projet, 1 Mio au plus chacun ; les caractères de contrôle sont affichés, pas envoyés au terminal |
| `tree [-a] [-L n] [chemin]` | l'arbre d'un répertoire, trois niveaux par défaut, 2000 entrées au plus ; `.webfront/` n'est ouvert que s'il est nommé |
| `mounts` | ce que l'application sert au navigateur, couche par couche et dans l'ordre où elle cherche, et ce que les commandes lisent |

Elles lisent le répertoire du projet par les callbacks qu'utilise wf (`developer::Store`) : les chemins sont relatifs au
projet et n'en sortent pas. Ce qu'elles affichent n'est pas de confiance : un dépôt cloné choisit ses noms de fichiers
et leur contenu, un paquet npm son `package.json`. Tout caractère de contrôle (C0, DEL et les C1 sur 8 bits, dont le CSI
U+009B) est donc rendu visible avant d'atteindre le terminal, dans ces commandes comme dans `wf list`, `wf why` et
`wf info`. `mounts` répond à la question de la nature du système de fichiers : les ressources embarquées
dans l'exécutable, les publications de `.webfront/developer/` (`environment`, `preview`, `test`), les providers propres
à l'application, et le fait que les sources du projet ne sont jamais servies. Les couches sont décrites, pas énumérées :
un `fs::Provider` n'a que `open()`, et lui ajouter une énumération aurait étendu l'API publique du cœur.

`ls -l` s'appuie sur un callback ajouté, `webfront.developer.entries`. C'est un ajout : la version d'API de
l'environnement reste 1, et un environnement récent chargé par une bibliothèque qui ne l'a pas le dit pour `ls -l` et
fonctionne pour le reste.

### 3. Les noms de fichiers non fournis sont réservés dès maintenant

`cd`, `pwd`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `touch`, `edit`, `find`, `grep`, `head`, `tail`, `less`, `stat`, `du` et
`df` sont réservés sans être fournis. Chacun répond par une erreur qui dit quoi faire : il n'y a pas de répertoire
courant ; le terminal ne modifie pas les fichiers, qui s'éditent dans l'éditeur du développeur pendant que `wf dev`
reconstruit.

Réserver un nom sans fournir la commande a un coût immédiat et certain : une application ne peut plus appeler une de
ses commandes `find`, `stat`, `edit` ou `head`, et `commands.add` lève `std::invalid_argument` pour celles qui le
faisaient. Ce coût est accepté pour trois raisons. Il se paie une seule fois, avant la 1.0, quand aucune application
publiée n'a de contrat à faire respecter ; la même réserve décidée après la 1.0 serait une rupture à chaque commande
ajoutée, donc ne serait jamais décidée, et le terminal resterait figé à quatre commandes. Les noms retenus sont ceux
qu'un développeur tape par réflexe dans un terminal : une application qui les prendrait pour autre chose tromperait
son utilisateur, et le terminal ne pourrait plus dire pourquoi `rm` ne fait rien. Enfin la liste est fermée et
limitée au vocabulaire des fichiers : les mots du domaine d'une application (`status`, `echo`, `print`, `count`…)
restent libres, et une commande nue future ne peut venir que de cette réserve.

`echo` n'est pas réservé : ce n'est pas une commande de fichiers, et c'est un nom naturel pour une commande
d'application.

Le terminal n'est toujours pas un shell : aucun processus n'est lancé, il n'y a ni tube, ni redirection, ni motif, ni
variable, ni répertoire courant. La mention « No system shell is exposed » reste exacte.

### 4. Le graphe des dépendances se lit avec wf

Le lock enregistre déjà, pour chaque paquet, le paquet exact auquel chacune de ses dépendances et chacun de ses pairs
satisfaits a été résolu. Trois commandes le lisent, sans réseau ni écriture :

- `wf list --tree` : les paquets sous les racines du manifeste, avec la plage qui les demande ; un paquet déjà ouvert
  est marqué `deduped`. `wf list` seul reste la liste plate.
- `wf why <paquet>` : les chaînes d'exigences qui amènent un paquet depuis les racines, pour chacune de ses versions.
- `wf info <paquet>` : licence, intégrité, fichiers, dépendances, dépendants et entrées exécutables d'un paquet verrouillé.

Elles n'ont pas d'opération d'automatisation, comme `wf list` : un agent lit le lock lui-même.

## Options écartées

| Option | Pourquoi |
| --- | --- |
| Supprimer le préfixe | Réserve quatorze mots courants, rend chaque nouveau verbe incompatible, rompt les scripts de manifeste, le pilote, la documentation et les environnements publiés. |
| Préfixe facultatif (`test` exécute `wf test` si l'application n'a pas de commande `test`) | Le sens d'une ligne dépend de l'application ; une commande ajoutée par l'application change silencieusement ce qu'une ligne exécutait. |
| Commandes de fichiers sous `wf` (`wf ls`) | Mêle l'inspection générique de fichiers au vocabulaire du projet, et prive des noms que tout le monde tape. |
| Écriture depuis le terminal (`cp`, `mv`, `rm`, `mkdir`) | `rm` et `mv` demandent des callbacks destructifs nouveaux ; aucune n'est nécessaire au développement, que l'éditeur et wf couvrent. Les noms sont réservés pour pouvoir y revenir. |
| Éditeur intégré (`edit`) | Un chantier à part entière, sans rapport avec une décision de nommage ; le nom est réservé. |
| Énumérer le système de fichiers servi | Étend le concept `fs::Provider` et les providers générés par `wf embed` juste avant la 1.0, pour une information que `mounts` donne déjà. |

## Conséquences

- **Contrat :** `webfront::commands::reservedNames` compte 26 noms. Une application qui enregistrait une commande sous
  l'un des nouveaux (`ls`, `cat`, `tree`, `mounts` ou un nom de fichiers réservé) doit la renommer : c'est la rupture
  que cette décision choisit de faire avant la 1.0 plutôt qu'après.
- **Règle pour la suite :** une commande qui lit ou modifie le manifeste, le lock, les paquets ou la construction entre
  sous `wf` ; une commande nue nouvelle ne peut venir que de la réserve.
- **Dégradation :** l'environnement reconnaît une bibliothèque sans `webfront.developer.entries` au texte d'erreur du
  bridge pour un callback non enregistré ; le parcours navigateur vérifie ce texte sur le vrai bridge.
- **Preuves :** specs Jasmine `files-specs.mjs` et `dependencies-specs.mjs`, scénarios Catch2 de `Store::entries` et
  des noms réservés, test `CommandNamesAgree`, et le parcours navigateur `developer_integration` qui exécute les commandes
  dans le terminal.
