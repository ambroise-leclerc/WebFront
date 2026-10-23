# ADR-013 : Amorcer un environnement développeur téléchargé à la demande

## Versions

| Version | Date | État | Auteur | Évolution | Commit |
| --- | --- | --- | --- | --- | --- |
| 0.1 | 2026-10-20 | Proposée | Ambroise Leclerc | Proposition et première implémentation du bootstrap développeur versionné. | À renseigner après intégration. |
| 0.2 | 2026-10-22 | Proposée | Ambroise Leclerc | Alignement sur ADR-016 : la proposition devient la base de l'architecture ; preuves multiplateformes manquantes listées. | [404fed2](https://github.com/ambroise-leclerc/WebFront/commit/404fed2c2145d4faba8efee035e10522ff87a91f) |
| 0.3 | 2026-10-22 | Proposée | Ambroise Leclerc | Provisioning local des paquets (#306) : `wf restore [--offline]`, priorité du registre de l'application sur celui du manifeste. | À renseigner après intégration. |
| 0.4 | 2026-10-24 | Proposée | Ambroise Leclerc | Retrait du starter PI ; Life reste le seul starter distribué, créé avec `wf create life`. | [8368f59](https://github.com/ambroise-leclerc/WebFront/commit/8368f595af1534ea6670ee6d5582f245f9edc1f2) |

## Statut et date

**Proposée le 2026-10-20.** L'implémentation expérimentale est soumise à validation ; la
distribution publique de l'environnement et la qualification des autres moteurs restent ouvertes.
La date est celle de la proposition, pas une origine historique reconstituée.

## Contexte

Une petite application C++ doit pouvoir acquérir son environnement de développement web sans
embarquer terminal, React, Babel ou compilateur JavaScript, et sans demander l'installation de npm.
Le navigateur possède déjà HTTPS et les primitives nécessaires au téléchargement.

## Décision proposée

`NativeApp` est une façade native explicite accessible depuis `WebFront.hpp`, sans modifier le
navigateur système par défaut. La création et `show()` ouvrent la fenêtre ; l'application enregistre
séparément ses callbacks et appelle `bootstrap("developer", options)` avant `show()`.
Le projet local est désigné explicitement lors de la construction.

Cette proposition introduit une exception de développement explicite à l'ADR-009 : le bootstrap
peut contacter un dépôt au lancement après activation volontaire. Les applications ordinaires et
le mode distribution conservent leur lancement local sans dépendance npm ou réseau.

Le petit chargeur embarqué télécharge une archive npm contenant le terminal et les outils. Il
vérifie une empreinte SHA-512 fournie par l'application, puis le manifeste de protocole 1 et les
chemins. Il publie les fichiers sous l'origine locale avant d'importer le module d'entrée.
L'archive mise en cache est revérifiée à chaque bootstrap ; une erreur affiche un diagnostic et
permet de réessayer. Un hash incompatible interdit le repli silencieux vers un autre environnement.

Le distributeur fournit URL HTTPS/CORS et empreinte par un canal de confiance. Un registre npm
peut héberger cette archive, sans client npm chez l'utilisateur et sans serveur spécifique WebFront.
La première distribution vise GitHub Pages, avec des chemins `developer/<version>/` conservés
et une configuration URL/SHA-512 épinglée dans `DeveloperReleaseData.hpp`. Le workflow refuse de
remplacer les octets d'une version publiée et archive les versions taguées dans des prereleases
séparées de la bibliothèque C++. Les archives gardent des identifiants de paquet, sans exiger
publication npm ou propriété d'un scope. Le manifeste vérifié épingle aussi un starter Life distinct (le starter PI initial a été retiré) ;
`wf create life` crée son dossier sans remplacer un projet existant.

## Alignement sur l'ADR-016

[ADR-016](ADR-016-bootstrap-comme-socle.md) retient cette proposition comme base de l'architecture.
Elle reste **Proposée** jusqu'aux preuves de #316 : parcours complet validé sous CEF, WebKitGTK,
WebView2 et WKWebView, compatibilité d'un échantillon de paquets npm, restauration hors ligne et
publication vérifiée d'une release. Les providers et composants historiques évoqués ici suivent la
trajectoire de retrait de l'ADR-016.

## Alternatives et conséquences

- Embarquer l'environnement complet : écarté pour conserver une application initiale légère.
- Piloter Node/npm installé : écarté pour l'autonomie du développeur utilisateur.
- Exécuter une page distante avec le bridge : écarté ; l'environnement validé est servi localement.
- Le premier lancement nécessite l'accès au dépôt ou un provisioning local explicite. Le cache
  vérifié autorise ensuite un lancement hors ligne.
- Les paquets suivent la même règle : `wf restore` reconstruit leur cache depuis le lock, depuis le
  registre fixé par l'application (canal de confiance du distributeur), sinon celui du manifeste, sinon
  registry.npmjs.org, toujours en HTTPS ou sur l'origine de l'application. Un registre servi par
  l'application est leur provisioning local explicite ; `wf restore --offline` vérifie le cache sans réseau.
- Le code téléchargé possède l'autorité du bridge et du projet autorisé ; ce n'est pas un sandbox.
  Le mode distribution n'active pas ces callbacks. Aucun accès au shell n'est ajouté.

## Règles et vérification

Conserver le bridge protégé, les Promises d'erreur et les transferts binaires existants. Ne pas
ajouter terminal/compiler/frameworks à `DefaultFS`. Ne pas activer implicitement une racine projet.
Tester intégrité incorrecte, cache hors ligne, protocole inconnu, chemins invalides et interruption.
Le test natif charge l'archive depuis un dépôt local ; aucun test ne dépend du registre public.

## Références

- [Environnement développeur](../DEVELOPER_ENVIRONMENT.md), [ADR-008](ADR-008-contenu-de-confiance-et-origines.md),
  [ADR-010](ADR-010-composants-et-outils-optionnels.md).
