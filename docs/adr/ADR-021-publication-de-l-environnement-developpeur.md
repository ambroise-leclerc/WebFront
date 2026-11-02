# ADR-021 : Simplifier la publication de l'environnement développeur

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-11-02 | Proposée | Ambroise Leclerc | Décision initiale : évaluation des méthodes de publication de l'environnement développeur après la publication de la 0.6.4 ; recommandation d'automatiser la chorégraphie actuelle et de ne pas installer de serveur dédié. | À renseigner après intégration. |

## État et chronologie

**Proposée** le **2026-11-02**, juste après la publication de l'environnement 0.6.4 (`developer-v0.6.4`).
Aucun code n'est modifié par cette version. Elle passera à « Acceptée » quand les critères de la section dédiée
seront démontrés par une publication réelle.

## Contexte

### Ce qui est publié, et où

L'environnement développeur (archive `environment.tgz`, starter Life, `provision.json`) est versionné
indépendamment de la bibliothèque C++ (`js/developer/tools/distribution.json`, tag `developer-v<version>`, par opposition
aux tags `v*`). Il est servi par GitHub Pages (branche `gh-pages`, 48,9 Mo au 2026-11-02), sous
`https://ambroise-leclerc.github.io/WebFront/developer/<version>/`. La bibliothèque compilée n'embarque que l'URL et
le SHA-512 de l'archive qu'elle accepte (`include/webfront/system/DeveloperReleaseData.hpp`, ADR-013, ADR-016) : une
application ne suit jamais `latest` et n'apprend jamais de hash de remplacement depuis un fichier distant.

### Cadence observée

Du 2026-10-22 au 2026-11-02, douze versions de l'environnement ont été publiées (0.4.1, 0.5.0, 0.6.0 à 0.6.4…) contre six
versions de la bibliothèque (0.4.0 à 0.8.0) : l'environnement évolue tant que `wf`, l'automatisation (ADR-019) et les
services système (ADR-020) se construisent, plus vite que la bibliothèque.

### La procédure actuelle

`PUBLISHING.md` décrit quatre temps pour une version, répartis sur plusieurs branches et pull requests :

1. fusionner les changements en laissant l'en-tête sur la version précédente (les sources avancent, le pin non) ;
2. sur une branche de release, `wf run build build-distribution --refresh-bootstrap`, commit du nouvel en-tête et des
   sommes de `bundles.json` ;
3. tag `developer-v<version>` sur ce commit : le workflow reconstruit deux fois, compare, publie sur `gh-pages`, demande la
   construction de Pages et archive dans une prerelease ;
4. vérification manuelle des octets déployés (SHA-512, CORS, `developer_integration --public`), **puis seulement**
   fusion de la branche de release, pour que le bootstrap par défaut ne désigne jamais une archive indisponible ;
5. dans une pull request distincte, avancer `cmake/WebFrontBootstrap.cmake` (le bootstrap des tests).

### Pourquoi c'est compliqué et risqué

- **La version est écrite à trois endroits** qui doivent concorder : `distribution.json` (sources), `DeveloperReleaseData.hpp`
  (ce que les applications acceptent) et `WebFrontBootstrap.cmake` (ce que les tests exécutent), plus les sommes de
  `bundles.json`. Un oubli est détecté tard (CI) ou pas du tout.
- **Le pin précède l'existence de l'archive** sur la branche de release, et ne peut être fusionné qu'après sa publication :
  une fenêtre pendant laquelle `master`/`develop` et ce qui est déployé divergent, avec une PR en brouillon à tenir
  à jour (exemple : la 0.6.3, `docs/validation/developer-0.6.3-release.md`).
- **Les étapes de vérification sont manuelles** (octets déployés, CORS, test public) ; l'ordre est une convention, non
  une contrainte du système.
- **Une version publiée est immuable.** Une erreur détectée à l'étape 4 oblige à incrémenter (0.6.3 → 0.6.4), ce qui rend
  chaque raté coûteux et alimente la cadence ci-dessus.
- **Deux numérotations** (bibliothèque, environnement) sans table de compatibilité écrite hors du code : l'utilisateur
  ne sait pas quel environnement accompagne quelle bibliothèque autrement qu'en lisant l'en-tête.

### Ce qu'il faut conserver

1. L'hôte fournit la confiance : le hash vient de la bibliothèque compilée, pas du serveur (ADR-013, ADR-016).
2. Les versions publiées ne changent jamais d'octets.
3. L'archive est reconstruisible à l'identique, sans Node.js, à partir du dépôt.
4. L'archive est servie avec des en-têtes CORS corrects (la raison pour laquelle GitHub Pages et non les assets de release).
5. La liste des ressources du cœur et son budget de 150 Kio (ADR-016) restent inchangés.

## Options évaluées

| # | Option | Simplifie | Coût et risques | Verdict |
| --- | --- | --- | --- | --- |
| A | Statu quo | — | Chorégraphie manuelle décrite ci-dessus. | Écartée comme cible. |
| B | **Automatiser la chorégraphie** : un seul workflow `release-developer` (dispatch avec la version) qui incrémente, construit deux fois, commit l'en-tête, tague, publie, vérifie les octets déployés puis ouvre la PR de fusion (ou la fusionne si tout est vert). Un fichier unique de vérité lu par CMake et par l'en-tête. | Étapes 2 à 5, trois endroits → un. | Écrit dans le dépôt depuis le CI (jeton, protection de branche). Ne change ni le modèle de confiance ni l'hébergement. | **Retenue (phase 1).** |
| C | Fusionner l'environnement dans le cycle de la bibliothèque (même numéro, publié par le tag `v*`) | Une seule numérotation, une seule release. | L'environnement évoluerait à la cadence des releases C++, ou la bibliothèque à celle de l'environnement (12 en 12 jours) ; le pin resterait à calculer avant le tag. Contredit la décorrélation voulue par #315. | Écartée. |
| D | Embarquer l'archive dans la bibliothèque ou la livrer avec la release | Plus de téléchargement à l'exécution. | 4 Mo contre un budget de 150 Kio (ADR-016) ; l'ADR-013 existe pour l'éviter. | Écartée. |
| E | **Signer l'archive** (clé publique dans l'en-tête, signature publiée avec l'archive ; minisign ou Sigstore) | L'en-tête ne change qu'à la rotation de clé : plus de pin par version, plus de PR de fusion, plus de fenêtre de divergence. | Déplace la confiance vers une clé à garder hors ligne ; exige une politique de version minimale/maximale pour éviter le retour en arrière ou le gel (réflexions TUF) ; modifie le contrat « le hash vient de l'hôte » (ADR-013/016) ; la signature doit être vérifiée dans le navigateur par le chargeur de 150 Kio. | À instruire (phase 2) si B ne suffit pas. |
| F | Assets de release GitHub comme source primaire | Un seul mécanisme de publication. | Redirections et CORS non garantis (déjà écarté en 0.x). | Écartée. |
| G | **Serveur dédié chez OVHcloud** | Contrôle des en-têtes, du domaine, de la bande passante, des journaux. | Système à exploiter (correctifs, TLS, sauvegardes, supervision) ; point de défaillance unique pour **tout** nouveau démarrage d'application ; surface d'attaque sur la chaîne d'approvisionnement ; coût récurrent. Les besoins réels (4 Mo par version, fichiers statiques immuables) sont très en deçà des limites de Pages. | Écartée comme serveur. |
| H | **Domaine à soi devant l'hébergement statique** (nom propre en `CNAME` sur Pages, ou stockage objet statique OVHcloud) | L'URL compilée dans chaque application ne dépend plus du compte GitHub ; migration d'hébergeur possible sans nouvelle bibliothèque. | Un nom de domaine à renouveler ; le hash de l'archive reste la garantie, donc le domaine n'élargit pas la confiance. | **Retenue (phase 1, indépendante), à décider séparément.** |

### Sur le serveur OVHcloud

L'envisager n'est utile que si l'un de ces besoins apparaît : volumétrie ou limites de Pages atteintes (1 Go de site,
100 Go de bande passante par mois, souples), journaux de téléchargement requis, ou politique de conservation qu'un
hébergement statique ne tient pas. Aucun n'est observé : l'ensemble des versions pèse 48,9 Mo. Le problème posé est
une **procédure** compliquée, pas un **hébergement** insuffisant ; changer d'hébergeur ne supprimerait aucune des étapes
2 à 5. Si un jour il le faut, un stockage objet statique est préférable à un serveur applicatif.

## Décision

1. **Phase 1, B : un seul workflow de release de l'environnement**, qui remplace les étapes manuelles de `PUBLISHING.md`.
   Il reste soumis aux invariants de la section « Ce qu'il faut conserver » : reconstruction double, immuabilité, hash
   épinglé dans l'en-tête, aucune écriture d'un fichier hors de `permissions.publish`.
2. **Une source unique pour le pin** : `WebFrontBootstrap.cmake` lit la version, l'URL et l'intégrité d'un même fichier
   que celui qui produit `DeveloperReleaseData.hpp`, ou en est dérivé par un test `CTest` de cohérence qui échoue à
   la moindre divergence. Le bootstrap des tests peut rester volontairement en retard sur l'en-tête, mais ce retard devient
   explicite et borné (une version), non un troisième endroit à éditer à la main.
3. **La vérification du déploiement est une étape du workflow**, pas une convention : SHA-512 des octets servis, en-têtes
   CORS, `developer_integration --public`. La fusion du pin ne s'ouvre que si elle réussit ; un échec ne laisse aucune
   branche à synchroniser (la PR n'a pas été ouverte).
4. **Table de compatibilité écrite** dans `docs/DEVELOPER_ENVIRONMENT.md` : version de la bibliothèque → environnement
   épinglé → `api` minimale. Elle est générée par le workflow, pas tenue à la main.
5. **Phase 2 conditionnelle (E, signatures)** : à instruire dans une ADR distincte si, après la phase 1, le pin par version
   reste une contrainte (par exemple si des applications tierces doivent recevoir des correctifs de l'environnement sans
   recompilation). Elle ne se décide pas ici car elle change le modèle de confiance.
6. **Domaine propre (H)** : décision séparée du propriétaire du projet ; indépendante des points 1 à 4. Elle doit avoir
   lieu **avant** que des applications tierces compilent l'URL actuelle en grand nombre, et non après.
7. **Pas de serveur dédié** (G) tant qu'un besoin mesuré ne le justifie pas.

## Conséquences

Positives : une opération au lieu de quatre branches/PR ; plus de fenêtre de divergence à surveiller ; les erreurs détectées
avant la fusion ne coûtent plus un numéro de version quand la vérification précède l'ouverture de la PR ; un seul
endroit à relire lors d'une revue de release.

Négatives : le workflow doit pouvoir écrire dans le dépôt (jeton à portée limitée, contournement éventuel de la protection de
`develop`) ; sa première version doit être éprouvée sur une publication réelle ; le tag ne désigne plus un commit préparé à la
main mais un commit produit par le CI, à documenter.

Risques résiduels : la publication reste irréversible (un octet faux publié impose une nouvelle version) ; Pages reste un
point de défaillance unique, atténué seulement par l'archive de la prerelease et par l'option H.

## Critères d'acceptation

1. Une version publiée par le seul workflow, sans édition manuelle de fichier entre le déclenchement et la fusion.
2. Un test échoue si `distribution.json`, `DeveloperReleaseData.hpp` et `WebFrontBootstrap.cmake` divergent hors du retard borné.
3. Une publication volontairement échouée à l'étape de vérification n'ouvre aucune PR et ne modifie ni `develop` ni `master`.
4. `PUBLISHING.md` décrit le workflow et ses points de reprise ; l'ancienne procédure manuelle n'y figure plus que comme secours.
5. Les invariants (reconstruction identique, immuabilité, budget de 150 Kio) sont toujours vérifiés par CTest.

## Prescriptions d'implémentation

- Ne jamais remplacer les octets d'une version publiée ; ne jamais suivre `latest`.
- Le workflow s'appuie sur `wf run` et sur les scripts existants (`build.mjs`, `site.mjs`, `pin.mjs`) ; il n'introduit pas
  de runtime JavaScript sur le runner (garde-fou `NoNodeDependency`).
- Toute modification du format de `provision.json`, du chargeur ou de l'en-tête pin reste soumise à l'ADR-016 et à
  l'ADR-003 pour le protocole.
- Aucune attribution d'outil d'assistance dans les commits, tags, prereleases ou descriptions de PR produits par le workflow.

## Références

- [ADR-013](ADR-013-bootstrap-developpeur-a-la-demande.md), [ADR-016](ADR-016-bootstrap-comme-socle.md) : confiance par hash
  épinglé, liste des ressources du cœur et budget.
- [ADR-019](ADR-019-developpement-automatisable-et-agents.md) : API d'automatisation et exigence d'une version minimale
  d'environnement.
- `js/developer/tools/PUBLISHING.md`, `.github/workflows/DeveloperDistribution.yml`, `cmake/WebFrontBootstrap.cmake`,
  `docs/validation/developer-0.6.3-release.md`.
- Mesures : `gh release list` et `git tag` (2026-11-02), taille de `origin/gh-pages` (48,9 Mo).
