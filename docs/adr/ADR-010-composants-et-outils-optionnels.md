# ADR-010 : Étendre WebFront par des composants et outils facultatifs

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-10-14 | Acceptée | Ambroise Leclerc | Décision initiale : architecture des composants optionnels et premier terminal. | [40f5621](https://github.com/ambroise-leclerc/WebFront/commit/40f5621928fdd188f230bf08ff062f9557619046) |
| 1.1 | 2026-10-14 | Acceptée | Ambroise Leclerc | Abonnements UI multiples et injection du navigateur appelant dans les callbacks. | [12a846b](https://github.com/ambroise-leclerc/WebFront/commit/12a846b653154862b6c71d4640ba396148653017) |
| 1.2 | 2026-10-14 | Acceptée | Ambroise Leclerc | Magasin transactionnel et fournisseur de fichiers des outils wf. | [4bcfc28](https://github.com/ambroise-leclerc/WebFront/commit/4bcfc28c7b78672603a2114a72d9decf25d36387) |
| 1.3 | 2026-10-15 | Acceptée | Ambroise Leclerc | Recherche, installation et ouverture des outils via une extension navigateur. | [18d5bb4](https://github.com/ambroise-leclerc/WebFront/commit/18d5bb4c5db6e4dc5ecf4867378d451887b2c133) |
| 1.4 | 2026-10-15 | Acceptée | Ambroise Leclerc | Validation des outils avec un registre local factice. | [d53ffcc](https://github.com/ambroise-leclerc/WebFront/commit/d53ffcc40102c7405eb7060d7021aad366d6d7e9) |
| 1.5 | 2026-10-16 | Acceptée | Ambroise Leclerc | Interruption des commandes navigateur et transferts séquentiels par blocs de 256 KiB. | [c81f7f7](https://github.com/ambroise-leclerc/WebFront/commit/c81f7f74728ad24a53c16f907e016eb0fe1b8bce) |
| 1.6 | 2026-10-22 | Acceptée | Ambroise Leclerc | Trajectoire de retrait de Terminal.hpp, ToolManager.hpp, ToolStore et PackageFS au profit de l'environnement bootstrap (ADR-016). | [404fed2](https://github.com/ambroise-leclerc/WebFront/commit/404fed2c2145d4faba8efee035e10522ff87a91f) |

## État et chronologie

**Acceptée.** Décision initiale : **2026-10-14** (version 1.0).
Dernière révision : **2026-10-22** (version 1.6).
Le présent texte expose la décision en vigueur ; le tableau retrace ses états antérieurs.

## Contexte

Le terminal apporte une interface réutilisable sans muer le cœur en framework DOM. Il fit
apparaître deux nécessités : plusieurs abonnements au démarrage de l’interface et
l’identification du navigateur appelant. Le gestionnaire `wf` étend ensuite cette voie aux
outils web distribués par npm.

## Décision

Que chaque composant réunisse une façade C++ sous `include/webfront/components/`, un fournisseur de
ressources embarquées, un module ES exportant `mount(element, options)` et les types du pont. Les noms de
callbacks utilisent `webfront.<composant>.<fonction>`. Les assets sont optionnels, absents de
`WebFront` tant que les providers correspondants ne sont pas sélectionnés.

Que le code réutilisable emploie `subscribeUIStarted` et conserve le `UISubscription`, sans
écraser l’unique handler `onUIStarted` de l’application. Le handle reste sûr après destruction
de l’application. Un premier paramètre de type `App::UI` dans un callback reçoit le navigateur
appelant ; il n’est pas décodé depuis les arguments JS.

Le terminal édite les lignes dans le navigateur et appelle les commandes C++. Les extensions
navigateur peuvent traiter une commande ou la laisser continuer côté C++. Leurs interruptions
ne promettent pas d’arrêter une commande C++.

Pour `wf`, HTTPS et vérification SHA-512 se font dans le navigateur. C++ valide les chemins,
reçoit les chunks séquentiels et installe via staging puis commit. `ToolStore` conserve
`webfront-tools.lock.json` et les fichiers sous `.webfront/tools/`. `PackageFS` utilise le même
emplacement projet que le gestionnaire. L’installation n’exécute pas de scripts npm et ne résout
pas les dépendances : un outil doit embarquer ce qu’il utilise. Il partage l’origine et
l’autorité du bridge de l’application ; c’est une fonction de développement de confiance.

## Options écartées et motifs

- Étendre le protocole pour chaque composant : évité, réutiliser les types du bridge.
- Écraser le handler UI global : remplacé par des abonnements à durée de vie explicite.
- Implémenter TLS ou un registre de paquets C++ dans le cœur : le navigateur fournit HTTPS
  et npm fournit métadonnées et distribution.
- Installer directement dans le répertoire final : rejeté au profit de transactions pour
  préserver la version installée en cas d’échec.

## Conséquences

- **Bénéfice :** extensions réutilisables et dépendances payées uniquement si sélectionnées.
- **Coût accepté :** plafond des chaînes du bridge, uploads séquentiels, sorties terminal
  C++ visibles à la fin de la commande ; aucune résolution de plages semver.
- **Risque :** un outil installé peut appeler tous les callbacks ; ce mécanisme ne remplace
  pas un système de plugins isolés. Staging et intégrité protègent l’installation, pas son code.

### Révision de l'ADR-016

`Terminal.hpp`, `ToolManager.hpp`, `ToolStore`, `PackageFS`, `TerminalFS`, `ToolManagerFS` et xterm
embarqué sont voués au retrait (#313) : le terminal et le `wf` de l'environnement téléchargé
([ADR-016](ADR-016-bootstrap-comme-socle.md)) les remplacent. Ils restent fonctionnels et testés
tant que l'équivalent n'est pas couvert ; aucune nouvelle fonctionnalité ne leur est ajoutée.
Les abonnements UI multiples et l'injection du navigateur appelant demeurent des contrats du cœur.

## Prescriptions d’implémentation et de génération de code

- Que registres et magasins soient possédés conjointement par les callbacks afin que la façade
  puisse disparaître avant l’application. Protéger les états utilisés par plusieurs threads.
- Que les préfixes réservés soient conservés et que `webFront.ready` soit attendu avant tout appel.
- Ne pas donner au composant un codec privé ; plafonner les sorties textuelles sous 65535 octets
  et utiliser les tableaux numériques pour les chunks binaires.
- Tester `wf` avec registre local factice et répertoire temporaire ; vérifier traversal, rollback,
  intégrité erronée et lockfile invalide, sans toucher aux outils de l’utilisateur.
- Que le fichier de verrouillage du projet soit versionné, mais non `.webfront/` ; qu’aucune
  authentification ni isolation des outils ne soit promise.

## Sources et vérification

- [Architecture des composants et limites](../UI_COMPONENTS.md),
  [Terminal](../../include/webfront/components/Terminal.hpp), [ToolManager](../../include/webfront/components/ToolManager.hpp).
- Tests : [UISubscribersTests](../../test/unit/UISubscribersTests.cpp),
  [ToolStoreTests](../../test/unit/ToolStoreTests.cpp), [tools-specs](../../test/web/js/tools-specs.mjs).
- Voir [ADR-002](ADR-002-systemes-de-fichiers-composables.md), [ADR-008](ADR-008-contenu-de-confiance-et-origines.md) et [ADR-016](ADR-016-bootstrap-comme-socle.md).
