# ADR-021 : Simplifier la publication de l'environnement développeur

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-11-02 | Acceptée | Ambroise Leclerc | Décision initiale : évaluation des méthodes de publication de l'environnement développeur après la publication de la 0.6.4 ; recommandation d'automatiser la chorégraphie actuelle, de ne pas installer de serveur dédié et de servir la distribution sous le domaine propre `dist.webfront.fr`. | À renseigner après intégration. |

## État et chronologie

**Acceptée** le **2026-11-02**, par le propriétaire du projet, après la publication de l'environnement 0.6.4
(`developer-v0.6.4`). L'acceptation vaut décision : le point 6 (domaine propre) est livré, la phase 1 (workflow de release
unique, source unique du pin, répétition avant publication, table de compatibilité, permissions par job) reste à
implémenter. Les critères de la section dédiée deviennent ses critères de réception ; ils sont à démontrer par une
publication réelle et à suivre dans l'issue qui la porte. Cette version ne modifie pas elle-même le code du dépôt.

**Domaine propre livré (point 6 de la décision, #478).** Le 2026-11-02, `dist.webfront.fr` est configuré (`CNAME` chez OVHcloud,
`webfront.fr` vérifié, certificat approuvé, HTTPS forcé) ; `site.mjs` écrit le `CNAME` (#479) ; l'environnement 0.6.5, première
version publiée sous ce domaine, est déployé, vérifié (octets, CORS, test public) et épinglé (#480) ; le bootstrap des
tests l'utilise (#481). Le reste de la décision (workflow de release unique, source unique du pin, répétition avant
publication, table de compatibilité) n'est pas réalisé.

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

`PUBLISHING.md` décrit cinq temps pour une version, répartis sur plusieurs branches et pull requests :

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
| H | **Domaine à soi devant l'hébergement statique** : `dist.webfront.fr` en `CNAME` sur Pages (ou, plus tard, stockage objet statique OVHcloud derrière le même nom) | L'URL compilée dans chaque application ne dépend plus du compte GitHub ; migration d'hébergeur possible en changeant le DNS, sans nouvelle bibliothèque. | `webfront.fr` doit être renouvelé et sa zone DNS protégée ; le hash de l'archive reste la garantie, donc le domaine n'élargit pas la confiance. | **Retenue (phase 1, indépendante des points B).** |

### Sur le serveur OVHcloud

L'envisager n'est utile que si l'un de ces besoins apparaît : volumétrie ou limites de Pages atteintes (1 Go de site,
100 Go de bande passante par mois, souples), journaux de téléchargement requis, ou politique de conservation qu'un
hébergement statique ne tient pas. Aucun n'est observé : l'ensemble des versions pèse 48,9 Mo. Le problème posé est
une **procédure** compliquée, pas un **hébergement** insuffisant ; changer d'hébergeur ne supprimerait aucune des étapes
2 à 5. Si un jour il le faut, un stockage objet statique est préférable à un serveur applicatif.

## Décision

1. **Phase 1, B : un seul workflow de release de l'environnement**, qui remplace les étapes manuelles de `PUBLISHING.md`.
   Il reste soumis aux invariants de la section « Ce qu'il faut conserver » : reconstruction double, immuabilité, hash
   épinglé dans l'en-tête. Deux périmètres d'écriture sont distingués : celui des **scripts `wf`**, qui n'écrivent que
   là où `permissions.publish` de `js/developer/webfront.project.json` l'autorise (inchangé), et celui de l'**étape de
   commit du workflow**, qui écrit sur la branche de release une liste fermée de chemins : `distribution.json`,
   `DeveloperReleaseData.hpp`, les sommes de `bundles.json`, les bundles régénérés et la table de compatibilité de
   `docs/DEVELOPER_ENVIRONMENT.md`. Cette liste est déclarée dans le workflow, et l'étape échoue si le diff en touche un
   autre. Les permissions se donnent **par job**, jamais pour tout le workflow, et la protection de `develop` et `master`
   n'est affaiblie par aucune :

   | Job | Permissions du jeton | Destination | Réglage du dépôt nécessaire |
   | --- | --- | --- | --- |
   | Construction et répétition (`wf-package`) | `contents: read` | aucune (artefact) | — |
   | Préparation | jeton de l'application (`contents: write`) | uniquement la branche `release/developer-<version>`, qui n'est ni `develop` ni `master` ; le commit se limite à la liste fermée ci-dessus | la branche n'est pas protégée par le motif de `develop`/`master` ; aucune dérogation de contournement n'est accordée au workflow |
   | Publication | `contents: write`, `pages: write` | la branche `gh-pages` (sans force-push) et la demande de construction de Pages ; une prerelease | source Pages inchangée (`gh-pages`, racine) ; son environnement `developer-pages-publication` reste l'unique écrivain |
   | Création du tag | `contents: write` | le tag `developer-v<version>`, posé par le **même run** après le succès de la construction : un tag poussé avec le jeton du workflow ne déclencherait pas un autre run, d'où le job unique plutôt que le déclenchement par tag | une règle de tags sur `developer-v*` interdit la mise à jour et la suppression |
   | Vérification | `contents: read` | aucune | — |
   | Ouverture de la PR de fusion | jeton d'une **application GitHub dédiée** (`pull-requests: write`, `contents: write` sur le dépôt, rien d'autre) ; le `GITHUB_TOKEN` du job se limite à `contents: read` | une PR de la branche de release vers `develop` | l'application est installée sur le dépôt, son secret est celui d'un environnement protégé ; la PR n'est **pas** fusionnée par le workflow : la revue exigée par la protection de `develop` reste |

   **La PR et le push de la branche de release utilisent le jeton de l'application, pas le `GITHUB_TOKEN`.** Les événements
   produits avec le `GITHUB_TOKEN` ne déclenchent pas d'autres workflows (hors `workflow_dispatch` et
   `repository_dispatch`), et ceux qui le seraient resteraient en attente d'une approbation manuelle : une PR créée avec ce
   jeton n'exécuterait donc pas, ou pas sans intervention, `BuildAndTest.yml` ni `DeveloperDistribution.yml`, c'est-à-dire
   les contrôles requis pour fusionner. Avec le jeton d'application, la PR et la branche se comportent comme celles d'une
   personne : les contrôles s'exécutent normalement et la protection de `develop` reste entière. Le réglage « Allow GitHub
   Actions to create pull requests » n'est pas nécessaire et reste désactivé. La ligne « Préparation » ci-dessus pousse
   avec ce même jeton pour la même raison. Le tag et la publication, eux, restent dans le run et n'ont pas besoin de
   déclencher un autre workflow.
2. **Une source unique pour le pin** : `WebFrontBootstrap.cmake` lit la version, l'URL et l'intégrité d'un même fichier
   que celui qui produit `DeveloperReleaseData.hpp`, ou en est dérivé, et un test de cohérence distingue les états
   normaux des incohérences. Sont **normaux** : (a) `distribution.json` en avance sur l'en-tête, les sources préparant
   la version suivante tandis que l'en-tête épingle la dernière version publiée ; il faut alors que l'en-tête soit
   exactement celui que génère sa propre version et que l'archive déployée ait l'intégrité épinglée ; (b) `distribution.json` égal à l'en-tête, qui doit alors être
   identique à celui d'une construction fraîche ; (c) le bootstrap des tests égal à l'en-tête ou à une version
   publiée antérieure, mais jamais postérieure. Trois vérifications sont **distinctes** et ne se remplacent pas :
   (i) les **octets** : l'archive servie aujourd'hui par `baseUrl` a l'intégrité épinglée (ce que fait `wf run pin`,
   qui la récupère au domaine courant et non à l'URL de l'en-tête) ; (ii) l'**URL réellement épinglée** : elle est sur
   l'origine de `baseUrl` et répond 200 sans redirection ; (iii) l'**accès depuis un navigateur** : un `fetch` d'une page
   d'une autre origine sur cette URL, avec en-têtes CORS valides (`developer_integration --public`, qui part de l'en-tête).
   **Dans tous les états, (i), (ii) et (iii) sont exigées** pour l'URL réellement épinglée par l'en-tête ; une
   archive dont les octets sont bons à une autre adresse ne suffit pas, car ce sont les applications construites depuis
   `develop` qui démarrent depuis l'URL compilée. `wf run pin` échoue donc (il ne se contente plus d'avertir) quand
   l'URL épinglée n'est pas sur l'origine de `baseUrl`, redirige ou n'est pas servie avec CORS.
   **Seule exception, la dérogation de migration**, explicite, bornée et temporaire : lorsque `baseUrl` change,
   `distribution.json` déclare l'ancienne adresse (`previousBase`) ; tant que l'en-tête épingle une version publiée sous
   cette adresse, (ii) et (iii) sont remplacées par un avertissement qui nomme la dérogation. Elle prend fin d'elle-même :
   elle n'est acceptée que pour un en-tête dont la version précède la première version publiée sous la nouvelle adresse,
   et son champ doit être retiré à ce pin ; le workflow refuse une dérogation périmée, ou un en-tête hors adresse sans
   déclaration. La migration déjà acceptée (github.io vers `dist.webfront.fr`, #478) est l'unique cas actuel et est
   close depuis le pin de la 0.6.5. Est **incohérent** : un en-tête plus récent que `distribution.json`, un en-tête qui n'est pas celui de sa version, une archive
   déployée dont l'intégrité diffère, ou un bootstrap de tests en avance sur l'en-tête.
3. **La vérification du déploiement est une étape du workflow**, pas une convention : SHA-512 des octets servis, en-têtes
   CORS, `developer_integration --public`. La PR de fusion du pin ne s'ouvre que si elle réussit. Comme la vérification
   suit la publication et que les versions publiées sont immuables, un échec dû à une panne transitoire (réseau,
   construction de Pages) se reprend avec les mêmes octets, tandis qu'un échec révélant des octets ou une configuration
   incorrects impose une nouvelle version. Pour réduire ce second cas, une **répétition sans publication** (dispatch
   `publish=false`) exécute avant le tag la même suite sur les octets candidats servis en local, comme l'a fait la 0.6.3.
4. **Table de compatibilité écrite** dans `docs/DEVELOPER_ENVIRONMENT.md` : version de la bibliothèque → environnement
   épinglé → `api` minimale. Les deux cycles étant indépendants, le workflow de l'environnement ne peut pas la tenir
   seul : une version de la bibliothèque peut sortir sans qu'il s'exécute. Le contrat est donc partagé : (a) le workflow
   de l'environnement réécrit la ligne **« non publiée »** (l'environnement épinglé par `develop`) à chaque pin ;
   (b) la préparation d'une release de la bibliothèque **fige** cette ligne en une ligne portant sa version, d'après
   l'en-tête `DeveloperReleaseData.hpp` du commit de release (génération par un script, pas à la main) ;
   (c) `CheckReleaseVersion.cmake`, déjà exécuté par `ReleaseVersion.yml` sur les tags `v*`, échoue si la version de la
   bibliothèque n'a pas sa ligne ou si la ligne diffère de l'en-tête. Une version sortie sans nouvelle publication de
   l'environnement reprend ainsi le même environnement que la précédente, sans attendre la suivante.
5. **Phase 2 conditionnelle (E, signatures)** : à instruire dans une ADR distincte si, après la phase 1, le pin par version
   reste une contrainte (par exemple si des applications tierces doivent recevoir des correctifs de l'environnement sans
   recompilation). Elle ne se décide pas ici car elle change le modèle de confiance.
6. **Domaine propre (H) : `dist.webfront.fr`**, à adopter au plus vite, indépendamment des points 1 à 4 et avant que des
   applications tierces compilent l'URL actuelle. Un sous-domaine dédié est utilisé parce que `webfront.fr` porte déjà le
   courrier (enregistrements MX chez OVHcloud) : le courrier n'est pas touché. Il reste le seul nom compilé dans les
   en-têtes ; GitHub Pages n'est qu'un hébergement derrière lui. Procédure :
   1. zone DNS : `dist` en `CNAME` vers `ambroise-leclerc.github.io.` ; si des enregistrements `CAA` existent, y autoriser
      `letsencrypt.org` ;
   2. vérification du domaine dans les paramètres Pages du compte GitHub (enregistrement `TXT` `_github-pages-challenge-…`),
      pour qu'aucun tiers ne puisse revendiquer un sous-domaine de `webfront.fr` ;
   3. activation sur le dépôt (`gh api -X PUT repos/ambroise-leclerc/WebFront/pages -f cname=dist.webfront.fr`), puis
      HTTPS forcé une fois le certificat émis ;
   4. `js/developer/scripts/site.mjs` écrit aujourd'hui seulement `.nojekyll` : il doit aussi écrire (ou conserver) le
      fichier `CNAME` de la branche `gh-pages` d'après `baseUrl`. Le workflow part d'un extrait de la branche existante, donc
      ne supprimerait pas un `CNAME` déjà présent ; l'écrire à chaque publication rend toutefois le domaine reproductible
      (branche recréée, extrait perdu) au lieu de dépendre d'un réglage fait une fois dans l'interface (#478) ;
   5. contrôle des anciennes URL : les versions 0.6.0 à 0.6.4 sont compilées avec
      `ambroise-leclerc.github.io/WebFront/…`, que Pages redirige ensuite vers le nouveau domaine ; un `fetch` de navigateur
      doit suivre cette redirection avec des en-têtes CORS valides (`developer_integration --public` sur une ancienne
      version). Les hash ne changent pas. **Résultat mesuré le 2026-11-02** : la redirection `301` de Pages ne porte pas
      d'en-tête `Access-Control-Allow-Origin` (seule la réponse finale en porte un) et Firefox, depuis une page en
      `http://127.0.0.1`, échoue en `NetworkError` sur l'ancienne URL, alors que l'URL du nouveau domaine répond 200.
      Pages ne permet pas d'ajouter cet en-tête à une redirection. **Décision** : le propriétaire accepte de rompre les
      binaires déjà compilés avec l'ancienne URL (bibliothèque 0.6.0 à 0.8.0, pré-alpha, sans compatibilité promise) plutôt
      que de renoncer au domaine ; ils devront être recompilés avec une version qui épingle `dist.webfront.fr` ;
   6. nouvelle version de l'environnement avec `baseUrl` à `https://dist.webfront.fr/` (changer une URL publique impose
      d'incrémenter la version), puis mise à jour de `WebFrontBootstrap.cmake`, de `docs/DEVELOPER_ENVIRONMENT.md` et
      de `js/developer/webfront.project.json` ; les anciennes versions restent à leur place.
7. **Pas de serveur dédié** (G) tant qu'un besoin mesuré ne le justifie pas.

## Conséquences

Positives : une opération au lieu de quatre branches/PR ; plus de fenêtre de divergence à surveiller ; les erreurs
détectables sur les octets candidats sont attrapées par la répétition avant publication et ne consomment pas de numéro ;
un seul endroit à relire lors d'une revue de release.

Négatives : le workflow doit pouvoir écrire dans le dépôt (application GitHub dédiée à portée limitée, dont il faut garder le secret ;
aucun contournement de la protection de `develop`) ; sa première version doit être éprouvée sur une publication réelle ; le tag ne désigne plus un commit préparé à la
main mais un commit produit par le CI, à documenter.

Rupture assumée : les applications compilées avec l'ancienne URL de Pages ne démarrent plus leur environnement
développeur une fois le domaine propre activé (redirection sans en-têtes CORS). Les versions de l'environnement déjà
publiées restent à leur emplacement, mais seule leur nouvelle URL est utilisable par un navigateur.

Risques résiduels : la publication reste irréversible (un octet ou une configuration incorrects constatés après
publication imposent une nouvelle version ; seule une panne transitoire se reprend avec les mêmes octets) ; Pages reste un
point de défaillance unique, atténué seulement par l'archive de la prerelease et par l'option H.

## Critères d'acceptation

1. Une version publiée par le seul workflow, sans édition manuelle de fichier entre le déclenchement et la fusion.
2. Le test de cohérence accepte les états normaux (a) à (c) de la décision 2, en particulier des sources en avance sur un
   en-tête resté sur la dernière version publiée, et échoue pour chacune des incohérences listées.
3. Une vérification volontairement échouée après publication n'ouvre aucune PR et ne modifie ni `develop` ni `master` ; une
   reprise avec les mêmes octets réussit après une panne simulée, et des octets incorrects mènent à une nouvelle version.
4. L'étape de commit échoue si le diff touche un chemin hors de la liste fermée de la décision 1, et chaque job du workflow
   déclare exactement les permissions du tableau de la décision 1 (un test du workflow le vérifie).
5. La PR de fusion ouverte par le workflow exécute `BuildAndTest.yml` et `DeveloperDistribution.yml` sans intervention
   manuelle, et le `GITHUB_TOKEN` n'a que `contents: read` dans ce job.
6. Un tag de bibliothèque `v*` dont la version n'a pas de ligne dans la table de compatibilité, ou dont la ligne diffère de
   l'en-tête, fait échouer `ReleaseVersion.yml`.
7. Un en-tête dont l'URL épinglée est hors de l'origine de `baseUrl`, redirigée ou sans CORS est refusé dans tous les états,
   y compris celui des sources en avance ; seule une dérogation de migration déclarée dans `distribution.json` le tolère,
   avec avertissement, et elle est refusée dès que l'en-tête épingle une version publiée sous la nouvelle adresse.
8. `PUBLISHING.md` décrit le workflow et ses points de reprise ; l'ancienne procédure manuelle n'y figure plus que comme secours.
9. Les invariants (reconstruction identique, immuabilité, budget de 150 Kio) sont toujours vérifiés par CTest.

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
