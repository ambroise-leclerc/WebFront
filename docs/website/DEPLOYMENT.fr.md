# Publication OVH — 27 octobre 2026

## Reconnexion et mises à jour

Favori vérifié dans Cyberduck le 29 octobre 2026 sur le Mac de développement :

- Favori à utiliser : **WebFront — OVH (SFTP)** (menu **Bookmark**, ou liste **Bookmarks**).
- Serveur : `ftp.cluster114.hosting.ovh.net`.
- Compte : `webfronth`.
- Protocole : SFTP, port `22`. Reconnexion testée avec succès sans nouvelle saisie du mot de passe, directement dans le dossier public. L'ancien favori **ftp.cluster114.hosting.ovh.net – FTP** est conservé, mais ne sert pas à la publication. La tentative FTP-SSL proposée par Cyberduck a échoué avec `500 This security scheme is not implemented` ; utiliser le favori SFTP fonctionnel.
- Dossier public : `www` depuis la racine de connexion, soit `/home/webfronth/www` sur l'hébergement.
- Le champ de mot de passe du favori est déjà renseigné. Réutiliser les identifiants enregistrés par Cyberduck ; ne jamais copier le mot de passe dans ce dépôt, les commandes ou les journaux. Une demande locale de déverrouillage ou d'autorisation macOS peut nécessiter l'intervention du propriétaire.
- URL web du favori : `https://webfront.fr/`.

Pour se reconnecter, ouvrir Cyberduck, afficher **Bookmark → Toggle Bookmarks**, puis ouvrir le favori ci-dessus. Aucune session dans l'espace client OVH n'est nécessaire pour réutiliser ce favori fonctionnel. En cas d'échec d'authentification, demander au propriétaire de vérifier ou de renouveler les identifiants directement dans Cyberduck ou dans **OVH → Web Cloud → Hébergements → FTP - SSH**.

Pour publier une mise à jour :

1. Depuis `docs/website`, lancer `node build.mjs`, puis `node check.mjs`.
2. Reconnecter le favori Cyberduck, ouvrir `www` et sauvegarder les fichiers publics qui seront remplacés dans `.deployment-backups/` (ignoré par Git).
3. Transférer les fichiers publics modifiés depuis `dist/`, en conservant les chemins relatifs. Préserver les fichiers historiques, la configuration serveur et le répertoire `/developer/` ; ne pas synchroniser par suppression.
4. Vérifier les pages françaises et anglaises, les ressources modifiées et le favicon sur `https://webfront.fr`, puis consigner le résultat de la publication ici.

### Publication du logo — 29 octobre 2026

L'hirondelle trichrome est publiée par le favori **WebFront — OVH (SFTP)**. Le transfert ciblé comprend 20 pages françaises et anglaises, `assets/site.css`, `assets/icon.svg` et `assets/webfront-swallow.svg`. Les originaux publics remplacés et le manifeste des empreintes SHA-256 sont conservés dans `.deployment-backups/logo-20261029/`, ignoré par Git. Le nouveau fichier `webfront-swallow.svg` était absent avant cette publication.

Cyberduck confirme le transfert terminé. Les 23 fichiers publiés ont été récupérés en HTTPS avec validation du certificat et comparés octet pour octet aux fichiers locaux. La page française a également été contrôlée dans le navigateur : le nouveau logo apparaît dans l'en-tête. Aucun fichier de `/developer/`, aucune règle serveur et aucun enregistrement DNS n'ont été modifiés.

## Opérations effectuées

- Offre confirmée dans l’espace client : hébergement gratuit, 100 Mo, cluster114, SFTP actif.
- Cyberduck installé via Homebrew ; le mot de passe a été saisi directement par le propriétaire, sans passer dans le chat.
- Sauvegarde complète des anciens fichiers de `www` téléchargée avant transfert. Archive locale : `.deployment-backups/ovh-www-20261027.tar.gz` (ignorée par Git).
- Site statique transféré dans `/home/webfronth/www` ; les autres fichiers historiques restent présents. L’ancienne page `index.html` figure dans la sauvegarde.
- Configuration Apache : `DirectoryIndex index.html`, désactivation du listing, page d’erreur 404, type JavaScript pour `.mjs`, et redirection 301 vers `https://webfront.fr` (HTTP et autres hostnames), avec exception pour les challenges ACME.
- URLs canoniques, alternances de langues, `sitemap.xml` et `robots.txt` préparés pour `https://webfront.fr`.
- Deux enregistrements DNS A modifiés : `webfront.fr` et `www.webfront.fr`, de `213.186.33.5` (redirection OVH) vers `213.186.33.87` (hébergement). Les réglages de messagerie, le sous-domaine `openai` et les autres entrées n’ont pas été modifiés.
- Les TXT de l’ancienne redirection sont conservés pour faciliter le retour arrière. Ils ne servent pas de redirection sur la nouvelle adresse d’hébergement.
- Certificats Let’s Encrypt demandés pour le domaine et `www`, avec accord du propriétaire. Les deux sont affichés « Active » dans OVH. La validation TLS réelle réussit sur le domaine principal ; celle de `www` reste en échec à la clôture de cette vérification. Aucune validation TLS n’est contournée.

## Vérification et état à confirmer

L’adresse technique `http://webfronth.cluster014.ovh.net/fr/` affiche le site. La version anglaise et l’avancement de la simulation ont été testés dans le navigateur.

Les serveurs DNS faisant autorité renvoient `213.186.33.87` pour les deux noms. Le résolveur local conserve encore `213.186.33.5` avec un TTL proche de 24 h : les visiteurs peuvent donc temporairement voir l’ancienne redirection.

Sur le serveur cible, les 30 fichiers publics ont été comparés octet pour octet à la version locale, en HTTPS avec validation du certificat de `webfront.fr`. Les types MIME JavaScript, le statut HTTP 404 et la redirection HTTP → HTTPS ont été vérifiés.

À recontrôler après propagation : accès normal sans résolution forcée et certificat TLS réellement présenté sur `www.webfront.fr`. OVH indique que la mise en place d’un certificat peut prendre plusieurs heures : https://docs.ovhcloud.com/fr/guides/web-cloud/web-hosting/ssl-letsencrypt .

## Retour arrière

Les relevés DNS avant et après modification sont conservés dans `.deployment-backups/`. Pour rétablir la redirection antérieure, remettre les deux A à `213.186.33.5` ; les TXT correspondants ont été conservés. Pour rétablir l’ancienne documentation sur l’hébergement, restaurer au minimum son `index.html` depuis l’archive. Ne pas restaurer tout le répertoire par suppression des nouveaux fichiers sans vérifier les changements intervenus depuis la publication.

## Mise à jour des releases et du livre — 31 octobre 2026

Publication des usages de WebFront 0.6.0 et 0.7.0 en français et en anglais britannique : nouveaux modèles d’IHM, harnais d’IA via MCP/CLI, parcours Jasmine sur une vraie page applicative, migration des options CMake. Le livre passe à huit chapitres et trois détours. Les sources de l’exemple Devis sont téléchargeables.

Transfert ciblé de 31 fichiers par le favori SFTP existant : 24 pages bilingues, CSS et JavaScript du site, sitemap et quatre fichiers de l’exemple. Les originaux remplacés et le manifeste SHA-256 sont conservés sous `.deployment-backups/releases-20261031/`. Aucun changement de DNS ou de configuration serveur ; aucun transfert dans `/developer/`.

Les fichiers publics ont été comparés en HTTPS avec validation TLS aux fichiers locaux. Le chapitre IA a été ouvert sur le domaine public. Voir [VALIDATION.fr.md](VALIDATION.fr.md) pour les validations du site, du tutoriel natif et de la suite C++ CEF désactivé.

## Révision rédactionnelle — 31 octobre 2026

La nouvelle rédaction bilingue et la convention de notation sont prêtes dans dist/. Le lot de transfert est `/private/tmp/webfront-editorial-publication-20261031/` : 24 pages et une feuille de style modifiées, ainsi que le sitemap conservé à l’identique. Les 25 originaux modifiés sont sauvegardés avec manifeste SHA-256 sous `.deployment-backups/editorial-20261031/`.

Le favori SFTP est reconnecté, mais le sélecteur Cyberduck conserve Upload désactivé. Les tentatives de sélection native ne déclenchent pas sa validation et le contrôle par coordonnées retourne `noWindowsAvailable`. Aucun transfert de cette révision n’a démarré. L’accès SFTP en mode batch par les clés déjà disponibles est refusé ; aucun mot de passe n’a été lu ou exporté.

Après rétablissement du dialogue, transférer le contenu du lot dans www, puis vérifier les 25 empreintes du manifeste sur le domaine HTTPS. Ne pas annoncer cette révision publiée avant cette vérification.

### Reprise et publication

Après remise de Cyberduck au premier plan par le propriétaire, le transfert a repris. La publication inclut la nouvelle présentation du framework, le cycle de développement et SiliconForge, ainsi que la révision rédactionnelle et typographique du livre. Cyberduck confirme 314,5 Ko transférés : les 24 pages bilingues et la feuille de style. Le sitemap, les exemples téléchargeables, la configuration serveur et les distributions n’ont pas été remplacés par ce transfert. Le contrôle HTTPS et la lecture de l’accueil public confirment le nouveau contenu.

## Introduction retenue et terminologie française — 31 octobre 2026

La proposition 3 (« Les applications graphiques C++ à l’ère de l’IA ») remplace le titre et l’introduction des deux accueils. Les textes français emploient « Opensource » et « Jeu de la vie » ; le nom Conway est conservé dans la version anglaise. Quatre pages changent : fr/index.html, en/index.html, fr/book/index.html et fr/book/tests/index.html. Leurs originaux et empreintes sont conservés sous `.deployment-backups/hero-20261031/`.

Le sélecteur Cyberduck étant resté désactivé, le transfert enregistré de la révision précédente a été relancé après actualisation des quatre fichiers dans son lot. Tous les autres fichiers de ce lot ont été comparés au répertoire dist avant la reprise : leurs contenus sont inchangés. Cette reprise conserve les mêmes chemins et n’effectue aucune suppression.

Cyberduck confirme le transfert terminé (315,2 Ko). Les quatre pages modifiées ont été récupérées en HTTPS avec validation TLS et comparées à leurs empreintes locales. L’accueil public a été vérifié dans le navigateur : titre, deux paragraphes retenus et libellé « Le Jeu de la vie ».
