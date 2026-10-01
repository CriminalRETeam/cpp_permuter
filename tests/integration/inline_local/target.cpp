struct Car { int speed; int gear; };
struct Driver { Car* car; int skill; };

int Boost(Driver* d, int amount)
{
    d->car->speed += amount * d->skill;
    d->car->gear = 2;
    return d->car->speed;
}
