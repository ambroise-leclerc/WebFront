# ADR-010 : Étendre par des composants optionnels et des outils de développement

## Versions

| Version | Date | État | Auteur | Évolution | Commit |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-10-14 | Acceptée | Ambroise Leclerc | Création initiale : architecture des composants optionnels et premier terminal. | [40f5621](https://github.com/ambroise-leclerc/WebFront/commit/40f5621928fdd188f230bf08ff062f9557619046) |
| 1.1 | 2026-10-14 | Acceptée | Ambroise Leclerc | Abonnements UI multiples et injection du navigateur appelant dans les callbacks. | [12a846b](https://github.com/ambroise-leclerc/WebFront/commit/12a846b653154862b6c71d4640ba396148653017) |
| 1.2 | 2026-10-14 | Acceptée | Ambroise Leclerc | Store transactionnel et provider de fichiers des outils wf. | [4bcfc28](https://github.com/ambroise-leclerc/WebFront/commit/4bcfc28c7b78672603a2114a72d9decf25d36387) |
| 1.3 | 2026-10-15 | Acceptée | Ambroise Leclerc | Recherche, installation et ouverture des outils via une extension navigateur. | [18d5bb4](https://github.com/ambroise-leclerc/WebFront/commit/18d5bb4c5db6e4dc5ecf4867378d451887b2c133) |
| 1.4 | 2026-10-15 | Acceptée | Ambroise Leclerc | Validation des outils avec un registre local factice. | [d53ffcc](https://github.com/ambroise-leclerc/WebFront/commit/d53ffcc40102c7405eb7060d7021aad366d6d7e9) |
| 1.5 | 2026-10-16 | Acceptée | Ambroise Leclerc | Interruption des commandes navigateur et uploads séquentiels par chunks de 256 KiB. | [c81f7f7](https://github.com/ambroise-leclerc/WebFront/commit/c81f7f74728ad24a53c16f907e016eb0fe1b8bce) |

## Statut et date

**Acceptée.** Création initiale : **2026-10-14** (version 1.0).
Dernière évolution : **2026-10-16** (version 1.5).
Le corps de l’ADR décrit la décision dans sa dernière version ; les versions antérieures
et leurs changements sont identifiés dans le tableau ci-dessus.

## Contexte

Le terminal introduit une interface réutilisable sans transformer le cœur en framework DOM.
Il révèle le besoin d’abonnements multiples au démarrage UI et d’identification du navigateur
appelant. Le gestionnaire `wf` prolonge ce composant pour installer des outils web npm.

## Décision

Un composant associe une façade C++ sous `include/components/`, un provider embarqué, un
module ES exportant `mount(element, options)` et les types existants du bridge. Les noms de
callbacks utilisent `webfront.<composant>.<fonction>`. Les assets sont optionnels, absents de
`WebFront` tant que les providers correspondants ne sont pas sélectionnés.

Le code réutilisable utilise `subscribeUIStarted` et conserve le `UISubscription`, sans
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

## Alternatives et arbitrage

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

## Règles d’implémentation et de génération de code

- Conserver les registres/stores par possession partagée dans les callbacks afin que la façade
  puisse disparaître avant l’application. Protéger les états utilisés par plusieurs threads.
- Garder les prefixes réservés et attendre `webFront.ready` avant les appels.
- Ne pas donner au composant un codec privé ; plafonner les sorties textuelles sous 65535 octets
  et utiliser les tableaux numériques pour les chunks binaires.
- Tester `wf` avec registre local factice et répertoire temporaire ; vérifier traversal, rollback,
  intégrité erronée et lockfile invalide, sans toucher aux outils de l’utilisateur.
- Versionner le lockfile de projet, pas `.webfront/`. Ne pas promettre d’authentification ou
  d’isolation des outils.

## Historique Git

- **2026-10-14** — [40f5621](https://github.com/ambroise-leclerc/WebFront/commit/40f5621928fdd188f230bf08ff062f9557619046) : terminal et architecture des composants optionnels.
- **2026-10-14** — [12a846b](https://github.com/ambroise-leclerc/WebFront/commit/12a846b653154862b6c71d4640ba396148653017) : abonnements UI et injection du navigateur appelant.
- **2026-10-14** — [3b8940c](https://github.com/ambroise-leclerc/WebFront/commit/3b8940c7a46d5d55fbfbe1c6870f417525cf044c) : correction du deadlock de désabonnement.
- **2026-10-14** — [4bcfc28](https://github.com/ambroise-leclerc/WebFront/commit/4bcfc28c7b78672603a2114a72d9decf25d36387) : store transactionnel et provider des outils.
- **2026-10-15** — [18d5bb4](https://github.com/ambroise-leclerc/WebFront/commit/18d5bb4c5db6e4dc5ecf4867378d451887b2c133) : recherche, installation et ouverture côté navigateur.
- **2026-10-15** — [d53ffcc](https://github.com/ambroise-leclerc/WebFront/commit/d53ffcc40102c7405eb7060d7021aad366d6d7e9) : tests contre un registre local.
- **2026-10-16** — [c81f7f7](https://github.com/ambroise-leclerc/WebFront/commit/c81f7f74728ad24a53c16f907e016eb0fe1b8bce) : interruption des commandes navigateur et chunks de 256 KiB.

## Références et vérification

- [Architecture des composants et limites](../UI_COMPONENTS.md),
  [Terminal](../../include/components/Terminal.hpp), [ToolManager](../../include/components/ToolManager.hpp).
- Tests : [UISubscribersTests](../../test/UISubscribersTests.cpp),
  [ToolStoreTests](../../test/ToolStoreTests.cpp), [tools-specs](../../webtest/js/tools-specs.mjs).
- Voir [ADR-002](ADR-002-systemes-de-fichiers-composables.md) et [ADR-008](ADR-008-contenu-de-confiance-et-origines.md).
