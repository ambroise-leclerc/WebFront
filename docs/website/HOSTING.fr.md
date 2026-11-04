# Hébergement de webfront.fr

Évaluation au 27 octobre 2026.

## Éléments confirmés et limites de la vérification

Les informations fournies depuis l’espace client indiquent : hébergement actif, 100 Mo de capacité, 3 Mo utilisés, trois sites configurés, datacentre eu-west-gra, cluster114. Le renouvellement est prévu en avril 2027. Aucun accès, mot de passe ou identifiant privé n’est nécessaire pour conserver ce constat. La vérification ultérieure dans l’espace client confirme l’offre « Free hosting », la racine commune `www` et l’absence initiale de certificat SSL. Voir [le journal de publication](DEPLOYMENT.fr.md) pour les opérations effectuées.

La capacité correspond à l’hébergement gratuit de 100 Mo décrit par OVHcloud, mais cette correspondance est une déduction, pas une identification contractuelle. La [présentation officielle de l’offre](https://www.ovhcloud.com/fr/domains/free-web-hosting/) indique 100 Mo, SSL et FTP. Le [guide d’activation OVH](https://docs.ovhcloud.com/fr/guides/web-cloud/web-hosting/activate-start10m) précise l’absence de base de données. Le site WebFront n’en utilise aucune.

## Décision recommandée

Conserver cet hébergement pour le site public et le livre. Le livrable statique initial représente environ 0,83 Mo de fichiers, images comprises (lancer `node check.mjs` pour la mesure exacte). Français et anglais sont déjà inclus. La simulation et les quiz s’exécutent chez le visiteur. Le serveur ne fait que distribuer HTML, CSS, JavaScript et images. Le nombre de chapitres ne nécessite donc pas un serveur applicatif.

Le quota disque convient largement. La performance réelle et le comportement du certificat doivent être testés après une mise en ligne provisoire ; les conditions exactes du contrat restent celles de l’espace client. Une simple redirection ne constitue pas une publication : il faudra servir les fichiers depuis la racine du domaine.

## Prévoir les packages sans migrer le site

| Usage | Infrastructure proposée | Quand évoluer |
| --- | --- | --- |
| Présentation, livre, démo locale | Hébergement statique actuel | Si le quota, la disponibilité ou les mesures de performance deviennent insuffisants |
| Quelques archives d’environnement versionnées | Distribution statique existante, séparée du site | Selon la taille cumulée et la fréquence des téléchargements |
| Beaucoup d’archives immuables et métadonnées de lecture | Stockage objet, domaine `packages.webfront.fr`, cache HTTP/CDN si utile | Quand le volume dépasse le budget du mutualisé ou que la diffusion le justifie |
| Registre acceptant des publications, comptes et droits | Service dédié sur VPS ou plateforme applicative, avec stockage objet pour les archives | Quand ces fonctions sont spécifiées et réellement nécessaires |

OVH propose un [Object Storage compatible S3](https://www.ovhcloud.com/fr/public-cloud/prices/) et des [VPS pour applications Node.js ou autres services](https://www.ovhcloud.com/fr/web-hosting/nodejs-hosting/). Le choix et le budget précis doivent dépendre du protocole, du volume stocké, du trafic sortant et de l’exploitation prévue. Un VPS ajoute mises à jour, supervision, sauvegardes et gestion des incidents. Il n’est pas justifié par le site actuel.

Un stockage objet n’est pas, à lui seul, un registre npm. WebFront installe aujourd’hui les packages compatibles navigateur depuis npm et vérifie leur intégrité. Un futur registre devra respecter le protocole réellement attendu par `wf`, fournir les métadonnées et archives cohérentes, gérer CORS/MIME/cache, conserver les versions immuables et leurs SHA-512, et tester les téléchargements, erreurs et restaurations hors ligne. L’authentification et les publications nécessitent un service d’écriture distinct si elles sont proposées.

Le dépôt possède déjà une publication statique de l’environnement développeur (`js/developer/scripts/site.mjs`). Il faut préserver ses URLs versionnées et les en-têtes bootstrap épinglés. Déplacer ces archives exige un plan de compatibilité, pas simplement une redirection du domaine principal.

## Procédure de mise en ligne

1. Identifier dans **Mes sites** la racine exacte de `webfront.fr` et `www.webfront.fr`, ainsi que les éventuels autres sites partageant les 100 Mo. Sauvegarder les fichiers et règles existants.
2. Vérifier dans **Certificats SSL** la couverture HTTPS du domaine et de `www`. Dans **FTP - SSH**, utiliser le mode de transfert chiffré disponible et un compte limité à la bonne racine.
3. Transférer le contenu de `dist/` dans une racine de prévisualisation isolée. Les chemins des assets commencent par `/` : utiliser un hostname de test pointant vers cette racine pour une vérification complète, pas un simple sous-répertoire sous un site existant.
4. Tester les deux langues, accès direct à `/fr/book/bridge/`, assets `.mjs` avec MIME JavaScript, démo, quiz, mobile et réponse 404. Aucune réécriture de type SPA n’est nécessaire. Configurer `ErrorDocument 404 /404.html` si Apache et l’offre l’autorisent.
5. Préparer les URLs canoniques absolues et le sitemap pour le domaine définitif. Puis remplacer la redirection GitHub par la publication, en conservant un retour arrière simple. Ne pas toucher aux chemins de packages existants.
6. Après bascule, tester HTTP → HTTPS et le hostname canonique, les liens externes, les pages indexables et les erreurs. Relever la taille et quelques mesures de chargement avant toute montée en gamme.

Les modifications de publication (transfert, DNS et demandes de certificats) sont décrites dans [DEPLOYMENT.fr.md](DEPLOYMENT.fr.md). Les packages existants n’ont pas été modifiés.
