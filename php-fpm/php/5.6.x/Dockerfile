FROM php:5.6-fpm
# Install modules
RUN apt-get update && apt-get install -y \
        libfreetype6-dev \
        libjpeg62-turbo-dev \
        libmcrypt-dev \
        libpng12-dev \
        libcurl4-openssl-dev  \
	libxml2-dev \
    && docker-php-ext-install iconv mcrypt \
    && docker-php-ext-install gd pdo_mysql mysqli curl  \
    && docker-php-ext-install mbstring json xml 
RUN mkdir -p /usr/local/etc/php 
COPY ./config/php.ini /usr/local/etc/php/php.ini
CMD ["php-fpm"]
