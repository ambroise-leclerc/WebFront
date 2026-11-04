# Mise à jour des usages — 31 octobre 2026

Références : notes de publication GitHub de v0.6.0 et v0.7.0, guides AUTOMATION, CONSUMER_TESTING, DOM, FORMS et REACT, qualification des modèles du 29 octobre. Les liens de référence du site ciblent le tag publié v0.7.0.

- Génération : 24 pages bilingues, entrée de langue et page 404 ; 24 URLs de sitemap.
- Vérification statique : 710 liens et ressources locaux ; tests de l’oscillateur, du bloc torique et du planeur conservés.
- Navigateur : chapitre IA, quiz, progression sur huit chapitres conservée entre langues et recherche anglaise du chapitre des parcours.
- Exemple Devis extrait des snippets du livre : compilation C++ avec Clang et WKWebView, deux specs Jasmine exécutées dans la page réelle, CTest réussi.
- Contrôle négatif : attente de 74 au lieu de 75 → CTest retourne 8, rapport Jasmine failed, une assertion en échec sur deux specs. Rétablissement de 75 → réussite.
- Dépôt : construction CEF désactivé dans build-382 ; suite CTest complète, 385/385 tests réussis. Aucun changement de bibliothèque ou de protocole.

Les sources téléchargeables sous examples/quote sont générées à partir des mêmes snippets que les pages. L’exemple publié cible macOS 13.3+ ; les instructions indiquent les moteurs à sélectionner sur les autres plateformes. Aucun facteur de productivité comparatif n’est inventé ; les résultats de qualification sont distingués d’un benchmark comparatif.

## Révision rédactionnelle et notation — 31 octobre 2026

- Réécriture des onze chapitres et des principales présentations du site en français et en anglais britannique ; paragraphes séparés par idée, listes de fonctions et titres descriptifs.
- Convention documentée dans STYLE.fr.md et présentée dans chaque chapitre : chasse fixe pour le code en ligne, italique pour un terme défini, légende indiquant le contexte d’exécution de chaque bloc.
- Vérification du rendu dans le navigateur : notice dépliable, liste des cinq fonctions Jasmine dans les deux langues, questionnaire et absence de débordement horizontal à une largeur de 595 px.
- Vérification statique : 26 pages éditoriales, 734 liens et ressources, 24 URLs de sitemap ; contrôle de l’échappement HTML et de la présentation des fonctions Jasmine.
- Les 29 exemples source restent identiques à ceux de la version publiée précédente. Les sources téléchargeables restent identiques également.
- Construction CEF désactivé puis suite CTest complète : 385/385 tests réussis.

Les 25 fichiers publics modifiés et leur sauvegarde sont prêts. Le transfert de cette révision n’a pas démarré : Cyberduck conserve son bouton Upload désactivé dans le sélecteur de fichiers ; la connexion SFTP par clé n’est pas autorisée sur cet hébergement. Le site public reste à la version antérieure jusqu’au transfert et à sa vérification HTTPS.

### Présentation du framework et publication

L’accueil et le début du livre présentent désormais la création d’applications et le cycle de développement complet. SiliconForge est décrit à partir de son README et du retour de son auteur ; les appréciations de rapidité ne sont pas présentées comme un benchmark comparatif. L’intégration d’un programme existant est maintenue comme une possibilité complémentaire.

Après rétablissement du dialogue Cyberduck par le propriétaire, les 25 fichiers modifiés ont été transférés. L’accueil public contient la nouvelle introduction et la section SiliconForge. Les contrôles du générateur, des 734 liens et de la notation restent réussis ; les 29 exemples exécutables restent identiques.
